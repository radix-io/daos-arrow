/*
 * Targeted test for GetFileInfo(FileSelector{recursive=true}) over nested
 * subdirectories -- PLAN.md calls this "the most novel piece" (it's what
 * FileSystemDatasetFactory uses to discover MPI-rank shards under data/,
 * and the thing CollectFileInfo's recursion in daos_arrow_fs.cc exists for).
 * test-daos-filesystem.cc only exercises a single flat file, which never
 * takes the recursive branch -- this test specifically does.
 *
 * Self-cleaning: removes the whole tree it builds on successful exit.
 */
#include "daos_arrow_fs.h"
#include <arrow/filesystem/filesystem.h>
#include <iostream>

int main() {
    std::string out_path;
    auto result = arrow::fs::FileSystemFromUri(
        "daos://radix-io/daos-s3-demo/claude-plan-test-recursive", &out_path);
    if (!result.ok()) {
        std::cerr << "FileSystemFromUri failed: " << result.status().ToString() << std::endl;
        return 1;
    }
    auto fs = *result;

    // Build: out_path/{a,b}/file.txt  (two subdirs, one file each)
    for (const char* sub : {"a", "b"}) {
        std::string dir = out_path + "/" + sub;
        auto st = fs->CreateDir(dir, /*recursive=*/true);
        if (!st.ok()) { std::cerr << "CreateDir(" << dir << ") failed: " << st.ToString() << std::endl; return 1; }
        auto os_result = fs->OpenOutputStream(dir + "/file.txt");
        if (!os_result.ok()) { std::cerr << "OpenOutputStream failed: " << os_result.status().ToString() << std::endl; return 1; }
        auto os = *os_result;
        std::string msg = std::string("contents of ") + sub + "\n";
        st = os->Write(msg.data(), msg.size());
        if (!st.ok()) { std::cerr << "Write failed: " << st.ToString() << std::endl; return 1; }
        st = os->Close();
        if (!st.ok()) { std::cerr << "Close failed: " << st.ToString() << std::endl; return 1; }
    }
    std::cout << "Built " << out_path << "/{a,b}/file.txt" << std::endl;

    // Non-recursive selector: should see only the two directories, not the files
    arrow::fs::FileSelector flat;
    flat.base_dir = out_path;
    flat.recursive = false;
    auto flat_result = fs->GetFileInfo(flat);
    if (!flat_result.ok()) { std::cerr << "GetFileInfo(flat) failed: " << flat_result.status().ToString() << std::endl; return 1; }
    std::cout << "Non-recursive listing (" << flat_result->size() << " entries):" << std::endl;
    for (auto& e : *flat_result) std::cout << "  " << e.path() << " (" << arrow::fs::ToString(e.type()) << ")" << std::endl;
    if (flat_result->size() != 2) {
        std::cerr << "EXPECTED 2 entries (a, b), got " << flat_result->size() << std::endl;
        return 1;
    }
    for (auto& e : *flat_result) {
        if (e.type() != arrow::fs::FileType::Directory) {
            std::cerr << "EXPECTED directory entries only in non-recursive listing" << std::endl;
            return 1;
        }
    }

    // Recursive selector: should see a/, a/file.txt, b/, b/file.txt (4 entries)
    arrow::fs::FileSelector rec;
    rec.base_dir = out_path;
    rec.recursive = true;
    auto rec_result = fs->GetFileInfo(rec);
    if (!rec_result.ok()) { std::cerr << "GetFileInfo(recursive) failed: " << rec_result.status().ToString() << std::endl; return 1; }
    std::cout << "Recursive listing (" << rec_result->size() << " entries):" << std::endl;
    int files = 0, dirs = 0;
    for (auto& e : *rec_result) {
        std::cout << "  " << e.path() << " (" << arrow::fs::ToString(e.type())
                  << (e.type() == arrow::fs::FileType::File ? ", size=" + std::to_string(e.size()) : "")
                  << ")" << std::endl;
        if (e.type() == arrow::fs::FileType::File) files++;
        else if (e.type() == arrow::fs::FileType::Directory) dirs++;
    }
    if (files != 2 || dirs != 2) {
        std::cerr << "EXPECTED 2 files + 2 dirs, got " << files << " files, " << dirs << " dirs" << std::endl;
        return 1;
    }
    std::cout << "RECURSIVE WALK OK: 2 subdirectories, 2 files, correctly typed" << std::endl;

    // cleanup
    for (const char* sub : {"a", "b"}) {
        std::string dir = out_path + "/" + sub;
        auto st = fs->DeleteFile(dir + "/file.txt");
        if (!st.ok()) { std::cerr << "cleanup DeleteFile failed: " << st.ToString() << std::endl; return 1; }
        st = fs->DeleteDir(dir);
        if (!st.ok()) { std::cerr << "cleanup DeleteDir(" << dir << ") failed: " << st.ToString() << std::endl; return 1; }
    }
    auto st = fs->DeleteDir(out_path);
    if (!st.ok()) { std::cerr << "cleanup DeleteDir(" << out_path << ") failed: " << st.ToString() << std::endl; return 1; }
    std::cout << "cleanup OK" << std::endl;

    std::cout << "ALL CHECKS PASSED" << std::endl;
    return 0;
}
