#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#c,errno); return 1; } } while (0)

static int size_is(const char *path, off_t size) {
    struct stat st;
    return !stat(path, &st) && st.st_size == size;
}

int main(int argc, char **argv) {
    CHECK(argc == 3);
    CHECK(unshare(CLONE_NEWNS | CLONE_NEWUSER) == 0);
    CHECK(mount(argv[1], "/cache-v3", NULL, MS_BIND, NULL) == 0);
    CHECK(size_is("/cache-v3/file", 1)); /* Fill the positive resolver cache. */
    CHECK(mount(argv[2], "/cache-v3", NULL, MS_BIND, NULL) == 0);
    CHECK(size_is("/cache-v3/file", 2)); /* A new exact mount replaces its source. */
    CHECK(umount2("/cache-v3", MNT_DETACH) == 0);
    CHECK(size_is("/cache-v3/file", 1)); /* Unmount exposes the prior source. */
    CHECK(rename("/cache-v3/file", "/cache-v3/renamed") == 0);
    struct stat st;
    errno = 0;
    CHECK(stat("/cache-v3/file", &st) == -1 && errno == ENOENT);
    CHECK(size_is("/cache-v3/renamed", 1));
    CHECK(unlink("/cache-v3/renamed") == 0);
    errno = 0;
    CHECK(stat("/cache-v3/renamed", &st) == -1 && errno == ENOENT);
    puts("PASS VFS resolver cache invalidates on overmount, unmount, rename and unlink");
    return 0;
}
