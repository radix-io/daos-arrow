/*
 * Step 1 sanity check (PLAN.md "Incremental build/verify order"): exercises
 * daos-cpp-dfs.h directly (no Arrow involved) against the real
 * radix-io/daos-s3-demo pool+container -- dfs_init, dfs_connect, dfs_lookup
 * of the root. Confirms the libdfs calls work in isolation before wiring
 * into the Arrow FileSystem backend. Run from a login node or compute node;
 * read-only, makes no changes to the container.
 */
#include "daos-cpp-dfs.h"
#include <cstdio>
#include <cerrno>

int main() {
    daos::fs::Library lib;
    if (lib.rc() != 0) {
        fprintf(stderr, "dfs_init failed: rc=%d (%s)\n", lib.rc(), strerror(lib.rc()));
        return 1;
    }
    printf("dfs_init OK\n");

    daos::fs::Mount m;
    int rc = m.connect("radix-io", "daos-s3-demo", O_RDWR);
    if (rc != 0) {
        fprintf(stderr, "dfs_connect failed: rc=%d (%s)\n", rc, strerror(rc));
        return 1;
    }
    printf("dfs_connect OK, dfs=%p\n", (void*)m.get());

    daos::fs::Obj root;
    struct stat st;
    rc = m.lookup("/", O_RDONLY, &root, &st);
    if (rc != 0) {
        fprintf(stderr, "lookup('/') failed: rc=%d (%s)\n", rc, strerror(rc));
        return 1;
    }
    printf("lookup('/') OK, mode=0%o size=%ld\n", st.st_mode, (long)st.st_size);
    return 0;
}
