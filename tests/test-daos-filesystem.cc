/*
 * exercises the real DaosFileSystem through Arrow's public FileSystem
 * interface (not daos-cpp-dfs.h directly) against the live
 * daos pool+container -- FileSystemFromUri registration,
 * CreateDir, OpenOutputStream/Write/Close, GetFileInfo (single path and
 * recursive FileSelector -- the latter is what
 * arrow::dataset::FileSystemDatasetFactory relies on in unified-reading.cc example),
 * OpenInputFile/Read round-trip, Move, CopyFile, DeleteFile, and the
 * NotFound (not error) semantics for a missing path.
 *
 * Self-cleaning: removes every file and the test directory it creates on
 * successful exit, so reruns don't accumulate state in the container.
 */
#include <arrow/buffer.h>
#include "daos_arrow_fs.h"
#include <arrow/filesystem/filesystem.h>
#include <arrow/io/interfaces.h>
#include <iostream>
#include <cstring>

int main() {
    // 1. Connect via FileSystemFromUri (registrar path)
    std::string out_path;
    auto result = arrow::fs::FileSystemFromUri(
        "daos://radix-io/daos-arrow-demo/claude-plan-test-fs", &out_path);
    if (!result.ok()) {
        std::cerr << "FileSystemFromUri failed: " << result.status().ToString() << std::endl;
        return 1;
    }
    auto fs = *result;
    std::cout << "FileSystemFromUri OK, type=" << fs->type_name()
              << " out_path=" << out_path << std::endl;

    // 2. CreateDir (recursive, since path is nested under root)
    auto st = fs->CreateDir(out_path, /*recursive=*/true);
    if (!st.ok()) { std::cerr << "CreateDir failed: " << st.ToString() << std::endl; return 1; }
    std::cout << "CreateDir(" << out_path << ") OK" << std::endl;

    // 3. OpenOutputStream, write bytes
    std::string filepath = out_path + "/hello2.txt";
    auto os_result = fs->OpenOutputStream(filepath);
    if (!os_result.ok()) { std::cerr << "OpenOutputStream failed: " << os_result.status().ToString() << std::endl; return 1; }
    auto os = *os_result;
    std::string msg = "hello from the real DaosFileSystem backend\n";
    st = os->Write(msg.data(), msg.size());
    if (!st.ok()) { std::cerr << "Write failed: " << st.ToString() << std::endl; return 1; }
    st = os->Close();
    if (!st.ok()) { std::cerr << "Close(output) failed: " << st.ToString() << std::endl; return 1; }
    std::cout << "OpenOutputStream + Write + Close OK (" << msg.size() << " bytes)" << std::endl;

    // 4. GetFileInfo on the file
    auto info_result = fs->GetFileInfo(filepath);
    if (!info_result.ok()) { std::cerr << "GetFileInfo failed: " << info_result.status().ToString() << std::endl; return 1; }
    auto info = *info_result;
    std::cout << "GetFileInfo: type=" << arrow::fs::ToString(info.type())
              << " size=" << info.size() << std::endl;
    if (info.type() != arrow::fs::FileType::File || info.size() != (int64_t)msg.size()) {
        std::cerr << "GetFileInfo MISMATCH" << std::endl;
        return 1;
    }

    // 5. OpenInputFile, read back, compare
    auto if_result = fs->OpenInputFile(filepath);
    if (!if_result.ok()) { std::cerr << "OpenInputFile failed: " << if_result.status().ToString() << std::endl; return 1; }
    auto infile = *if_result;
    auto read_result = infile->Read((int64_t)msg.size() + 10);
    if (!read_result.ok()) { std::cerr << "Read failed: " << read_result.status().ToString() << std::endl; return 1; }
    auto buf = *read_result;
    std::string readback(reinterpret_cast<const char*>(buf->data()), buf->size());
    std::cout << "Read back: \"" << readback << "\" (" << buf->size() << " bytes)" << std::endl;
    if (readback != msg) {
        std::cerr << "ROUNDTRIP MISMATCH" << std::endl;
        return 1;
    }
    std::cout << "ROUNDTRIP OK: byte-identical" << std::endl;
    st = infile->Close();

    // 6. GetFileInfo(FileSelector) -- the dataset-discovery path ex5 relies on
    arrow::fs::FileSelector sel;
    sel.base_dir = out_path;
    sel.recursive = true;
    auto sel_result = fs->GetFileInfo(sel);
    if (!sel_result.ok()) { std::cerr << "GetFileInfo(selector) failed: " << sel_result.status().ToString() << std::endl; return 1; }
    std::cout << "GetFileInfo(FileSelector recursive) found " << sel_result->size() << " entries:" << std::endl;
    for (auto& e : *sel_result) {
        std::cout << "  " << e.path() << " (" << arrow::fs::ToString(e.type()) << ")" << std::endl;
    }

    // 7. Move
    std::string moved = out_path + "/hello2-moved.txt";
    st = fs->Move(filepath, moved);
    if (!st.ok()) { std::cerr << "Move failed: " << st.ToString() << std::endl; return 1; }
    std::cout << "Move OK" << std::endl;
    auto moved_info = fs->GetFileInfo(moved);
    if (!moved_info.ok() || moved_info->type() != arrow::fs::FileType::File) {
        std::cerr << "Move verification failed" << std::endl;
        return 1;
    }
    std::cout << "Move verified: " << moved << " exists" << std::endl;

    // 8. CopyFile
    std::string copied = out_path + "/hello2-copy.txt";
    st = fs->CopyFile(moved, copied);
    if (!st.ok()) { std::cerr << "CopyFile failed: " << st.ToString() << std::endl; return 1; }
    auto copy_info = fs->GetFileInfo(copied);
    if (!copy_info.ok() || copy_info->size() != (int64_t)msg.size()) {
        std::cerr << "CopyFile verification failed" << std::endl;
        return 1;
    }
    std::cout << "CopyFile OK, size matches" << std::endl;

    // 9. DeleteFile cleanup
    st = fs->DeleteFile(moved);
    if (!st.ok()) { std::cerr << "DeleteFile(moved) failed: " << st.ToString() << std::endl; return 1; }
    st = fs->DeleteFile(copied);
    if (!st.ok()) { std::cerr << "DeleteFile(copied) failed: " << st.ToString() << std::endl; return 1; }
    std::cout << "DeleteFile cleanup OK" << std::endl;

    // 10. error path: GetFileInfo on nonexistent -> NotFound, not an error
    auto nf = fs->GetFileInfo(out_path + "/does-not-exist");
    if (!nf.ok()) { std::cerr << "GetFileInfo(nonexistent) returned error (should be NotFound): " << nf.status().ToString() << std::endl; return 1; }
    std::cout << "GetFileInfo(nonexistent) type=" << arrow::fs::ToString(nf->type()) << " (expect NotFound)" << std::endl;
    if (nf->type() != arrow::fs::FileType::NotFound) { std::cerr << "WRONG TYPE" << std::endl; return 1; }

    // 11. cleanup: remove the test directory (DeleteDir exercises
    // dfs_remove(..., force=true) on a non-empty dir -- there's nothing
    // left in it at this point, but it's the real cleanup path either way)
    st = fs->DeleteDir(out_path);
    if (!st.ok()) { std::cerr << "DeleteDir cleanup failed: " << st.ToString() << std::endl; return 1; }
    auto gone = fs->GetFileInfo(out_path);
    if (!gone.ok() || gone->type() != arrow::fs::FileType::NotFound) {
        std::cerr << "DeleteDir cleanup verification failed" << std::endl;
        return 1;
    }
    std::cout << "DeleteDir cleanup OK -- " << out_path << " removed" << std::endl;

    std::cout << "ALL CHECKS PASSED" << std::endl;
    return 0;
}
