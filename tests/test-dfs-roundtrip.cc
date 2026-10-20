/*
 * Step 2 round-trip test (PLAN.md "Incremental build/verify order"):
 * exercises daos-cpp-dfs.h's Mount wrapper directly -- mkdir, open-for-write,
 * write_at, get_size, lookup-for-read, read_at, readdir_all, and the ENOENT
 * error path -- against the real radix-io/daos-s3-demo pool+container.
 *
 * This is also where the "lookup parent O_RDONLY then try to create a child
 * under it -> EPERM" bug surfaced during plan execution; see the big
 * comment on daos::fs::Mount::lookup in daos-cpp-dfs.h. `lookup(dir)` below
 * is O_RDWR for that reason.
 *
 * Self-cleaning: removes its own test directory on successful exit, so
 * reruns don't accumulate files in the container.
 */
#include "daos-cpp-dfs.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>

#define CHECK(expr, label) do { \
    int _rc = (expr); \
    if (_rc != 0) { fprintf(stderr, "%s failed: rc=%d (%s)\n", label, _rc, strerror(_rc)); return 1; } \
} while (0)

static const char *kTestDirName = "claude-plan-test-dfs";
static const std::string kTestDirPath = std::string("/") + kTestDirName;
static const std::string kTestFilePath = kTestDirPath + "/hello.txt";

int main() {
    daos::fs::Library lib;
    CHECK(lib.rc(), "dfs_init");

    daos::fs::Mount m;
    CHECK(m.connect("radix-io", "daos-s3-demo", O_RDWR), "dfs_connect");

    // mkdir the test dir at root (ignore EEXIST from a prior run)
    int rc = m.mkdir(nullptr, kTestDirName, 0755);
    if (rc != 0 && rc != EEXIST) {
        fprintf(stderr, "mkdir failed: rc=%d (%s)\n", rc, strerror(rc));
        return 1;
    }
    printf("mkdir('%s') rc=%d (%s)\n", kTestDirName, rc, rc ? strerror(rc) : "ok/created");

    // NB: O_RDWR, not O_RDONLY -- creating a child (hello.txt) under this
    // directory requires the *parent* handle to carry the DAOS_OO_RW bit.
    daos::fs::Obj dir;
    CHECK(m.lookup(kTestDirPath, O_RDWR, &dir), "lookup(dir)");
    printf("lookup(dir) OK\n");

    // open file for write (O_CREAT|O_TRUNC|O_RDWR) under that dir
    daos::fs::Obj file;
    CHECK(m.open(dir.get(), "hello.txt", S_IFREG | 0644,
                 O_CREAT | O_TRUNC | O_RDWR, &file), "dfs_open(write)");
    printf("open(hello.txt, write) OK\n");

    const char *msg = "hello from daos-cpp-dfs.h plan execution\n";
    size_t msg_len = strlen(msg);
    daos_size_t written = 0;
    CHECK(m.write_at(file.get(), msg, msg_len, 0), "write_at");
    printf("write_at OK (%zu bytes)\n", msg_len);

    daos_size_t sz = 0;
    CHECK(m.get_size(file.get(), &sz), "get_size");
    printf("get_size after write: %ld\n", (long)sz);
    if ((size_t)sz != msg_len) {
        fprintf(stderr, "SIZE MISMATCH: wrote %zu, get_size reports %ld\n", msg_len, (long)sz);
        return 1;
    }

    // release write handle, reopen read-only (dfs doesn't require this but
    // it's a closer analog of close()+OpenInputFile() in the real backend)
    file.reset();

    daos::fs::Obj rfile;
    CHECK(m.lookup(kTestFilePath, O_RDONLY, &rfile), "lookup(file for read)");

    char buf[256] = {0};
    CHECK(m.read_at(rfile.get(), buf, sizeof(buf), 0, &written), "read_at");
    printf("read_at OK (%ld bytes): \"%s\"", (long)written, buf);

    if (written != msg_len || memcmp(buf, msg, msg_len) != 0) {
        fprintf(stderr, "ROUNDTRIP MISMATCH\n");
        return 1;
    }
    printf("ROUNDTRIP OK: byte-identical\n");
    rfile.reset();

    // readdir_all over the test dir (dir handle above is already O_RDWR,
    // which is also sufficient for reading -- no need to reopen)
    printf("readdir_all('%s'):\n", kTestDirPath.c_str());
    CHECK(daos::fs::readdir_all(m, dir.get(), [](const struct dirent& de) {
        printf("  entry: %s\n", de.d_name);
    }), "readdir_all");

    // error-path test: lookup a nonexistent file, confirm errno mapping
    daos::fs::Obj missing;
    rc = m.lookup(kTestDirPath + "/does-not-exist", O_RDONLY, &missing);
    printf("lookup(nonexistent) rc=%d, daos::fs::strerr=\"%s\" (expect ENOENT=%d)\n",
           rc, daos::fs::strerr(rc).c_str(), ENOENT);
    if (rc != ENOENT) {
        fprintf(stderr, "UNEXPECTED: expected ENOENT, got %d\n", rc);
        return 1;
    }

    // cleanup: remove the file, then the (now-empty) directory
    CHECK(m.remove(dir.get(), "hello.txt", /*force=*/false), "cleanup: remove(hello.txt)");
    dir.reset();
    CHECK(m.remove(nullptr, kTestDirName, /*force=*/false), "cleanup: remove(dir)");
    printf("cleanup OK -- %s removed\n", kTestDirPath.c_str());

    printf("ALL CHECKS PASSED\n");
    return 0;
}
