/*
 * exercises DaosFileSystemOptions (PLAN.md optimization #3): parsing
 * oclass/chunk_size off a daos:// URI's query string, and confirming the
 * values actually reach newly-created DAOS objects.
 *
 * - A file opened through a daos://...?chunk_size=N URI gets chunk size N
 *   (verified via dfs_get_chunk_size, exposed as Mount::get_chunk_size).
 * - A file opened through a daos://...?oclass=file:single URI resolves the
 *   hint (via dfs_suggest_oclass) and is created successfully -- there's no
 *   portable way to read an object's resolved class back out through the
 *   DFS API from here, so this leg checks observable behavior (create
 *   succeeds, round-trips correctly) rather than the class id itself.
 * - A plain daos:// URI (no query string) still defaults to options
 *   equivalent to DaosFileSystemOptions::Defaults(), i.e. 0/0 -- exercised
 *   already by test-daos-filesystem.cc; not repeated here.
 *
 * Self-cleaning: removes every file and the test directory it creates on
 * successful exit, so reruns don't accumulate state in the container.
 */
#include <arrow/buffer.h>
#include "daos_arrow_fs.h"
#include <arrow/filesystem/filesystem.h>
#include <arrow/io/interfaces.h>
#include <iostream>

int main() {
    // 1. chunk_size query param actually reaches the created object.
    std::string out_path;
    auto result = arrow::fs::FileSystemFromUri(
        "daos://radix-io/daos-arrow-demo/claude-plan-test-opts?chunk_size=4194304",
        &out_path);
    if (!result.ok()) {
        std::cerr << "FileSystemFromUri(chunk_size) failed: " << result.status().ToString()
                  << std::endl;
        return 1;
    }
    auto fs = *result;
    auto* daos_fs = static_cast<daos_arrow::DaosFileSystem*>(fs.get());
    if (daos_fs->options().chunk_size != 4194304) {
        std::cerr << "options().chunk_size mismatch: got "
                  << daos_fs->options().chunk_size << std::endl;
        return 1;
    }
    std::cout << "DaosFileSystemOptions.chunk_size parsed from URI OK (4194304)" << std::endl;

    auto st = fs->CreateDir(out_path, /*recursive=*/true);
    if (!st.ok()) { std::cerr << "CreateDir failed: " << st.ToString() << std::endl; return 1; }

    std::string filepath = out_path + "/chunked.txt";
    auto os_result = fs->OpenOutputStream(filepath);
    if (!os_result.ok()) {
        std::cerr << "OpenOutputStream failed: " << os_result.status().ToString() << std::endl;
        return 1;
    }
    auto os = *os_result;
    std::string msg = "exercising a non-default DAOS chunk size\n";
    st = os->Write(msg.data(), msg.size());
    if (!st.ok()) { std::cerr << "Write failed: " << st.ToString() << std::endl; return 1; }
    st = os->Close();
    if (!st.ok()) { std::cerr << "Close failed: " << st.ToString() << std::endl; return 1; }

    // Confirm the object DAOS actually created has the requested chunk
    // size, via a direct dfs_lookup + dfs_get_chunk_size -- bypassing
    // DaosFileSystem, since Arrow's FileSystem interface has no chunk-size
    // getter (there's no cross-backend concept of it to expose).
    {
        daos::fs::Library lib;
        if (lib.rc() != 0) { std::cerr << "dfs_init failed\n"; return 1; }
        daos::fs::Mount m;
        if (m.connect("radix-io", "daos-arrow-demo", O_RDWR) != 0) {
            std::cerr << "dfs_connect failed\n";
            return 1;
        }
        daos::fs::Obj obj;
        int rc = m.lookup("/" + filepath, O_RDONLY, &obj);
        if (rc != 0) { std::cerr << "lookup(chunked.txt) failed: rc=" << rc << std::endl; return 1; }
        daos_size_t csize = 0;
        rc = m.get_chunk_size(obj.get(), &csize);
        if (rc != 0) { std::cerr << "get_chunk_size failed: rc=" << rc << std::endl; return 1; }
        std::cout << "dfs_get_chunk_size on created file: " << csize << std::endl;
        if (csize != 4194304) {
            std::cerr << "CHUNK SIZE MISMATCH: expected 4194304, got " << csize << std::endl;
            return 1;
        }
    }
    std::cout << "chunk_size round-trip OK: object created with requested chunk size" << std::endl;

    // 2. oclass hint resolves and a file opens/writes/reads back fine.
    std::string out_path2;
    auto result2 = arrow::fs::FileSystemFromUri(
        "daos://radix-io/daos-arrow-demo/claude-plan-test-opts2?oclass=file:single",
        &out_path2);
    if (!result2.ok()) {
        std::cerr << "FileSystemFromUri(oclass) failed: " << result2.status().ToString()
                  << std::endl;
        return 1;
    }
    auto fs2 = *result2;
    auto* daos_fs2 = static_cast<daos_arrow::DaosFileSystem*>(fs2.get());
    if (daos_fs2->options().file_oclass == 0) {
        std::cerr << "options().file_oclass was not resolved from oclass hint (still 0)"
                  << std::endl;
        return 1;
    }
    std::cout << "oclass hint 'file:single' resolved to cid=" << daos_fs2->options().file_oclass
              << std::endl;

    st = fs2->CreateDir(out_path2, /*recursive=*/true);
    if (!st.ok()) { std::cerr << "CreateDir(2) failed: " << st.ToString() << std::endl; return 1; }
    std::string filepath2 = out_path2 + "/singled.txt";
    auto os2_result = fs2->OpenOutputStream(filepath2);
    if (!os2_result.ok()) {
        std::cerr << "OpenOutputStream(2) failed: " << os2_result.status().ToString()
                  << std::endl;
        return 1;
    }
    auto os2 = *os2_result;
    std::string msg2 = "exercising dfs_suggest_oclass(file:single)\n";
    st = os2->Write(msg2.data(), msg2.size());
    if (!st.ok()) { std::cerr << "Write(2) failed: " << st.ToString() << std::endl; return 1; }
    st = os2->Close();
    if (!st.ok()) { std::cerr << "Close(2) failed: " << st.ToString() << std::endl; return 1; }

    auto if2_result = fs2->OpenInputFile(filepath2);
    if (!if2_result.ok()) {
        std::cerr << "OpenInputFile(2) failed: " << if2_result.status().ToString() << std::endl;
        return 1;
    }
    auto buf2_result = (*if2_result)->Read((int64_t)msg2.size() + 10);
    if (!buf2_result.ok()) { std::cerr << "Read(2) failed\n"; return 1; }
    std::string readback2(reinterpret_cast<const char*>((*buf2_result)->data()),
                          (*buf2_result)->size());
    if (readback2 != msg2) { std::cerr << "oclass file ROUNDTRIP MISMATCH\n"; return 1; }
    std::cout << "oclass='file:single' create+write+read round-trip OK" << std::endl;

    // 3. Invalid chunk_size in the URI is rejected with a clear error.
    auto bad = arrow::fs::FileSystemFromUri(
        "daos://radix-io/daos-arrow-demo/x?chunk_size=not-a-number");
    if (bad.ok()) {
        std::cerr << "expected FileSystemFromUri to reject a non-numeric chunk_size\n";
        return 1;
    }
    std::cout << "invalid chunk_size correctly rejected: " << bad.status().ToString() << std::endl;

    // cleanup
    st = fs->DeleteFile(filepath);
    if (!st.ok()) { std::cerr << "DeleteFile cleanup failed: " << st.ToString() << std::endl; return 1; }
    st = fs->DeleteDir(out_path);
    if (!st.ok()) { std::cerr << "DeleteDir cleanup failed: " << st.ToString() << std::endl; return 1; }
    st = fs2->DeleteFile(filepath2);
    if (!st.ok()) { std::cerr << "DeleteFile(2) cleanup failed: " << st.ToString() << std::endl; return 1; }
    st = fs2->DeleteDir(out_path2);
    if (!st.ok()) { std::cerr << "DeleteDir(2) cleanup failed: " << st.ToString() << std::endl; return 1; }
    std::cout << "cleanup OK" << std::endl;

    std::cout << "ALL CHECKS PASSED" << std::endl;
    return 0;
}
