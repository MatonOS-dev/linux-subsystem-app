#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { perror(#x); return 1; } } while (0)

static int copy_links(int count) {
    char source[64], target[96], symlink_path[64];
    for (int i = 0; i < count; i++) {
        snprintf(source, sizeof(source), "src/file-%d", i);
        snprintf(target, sizeof(target), "var/tmp-copy/usr/lib/file-%d", i);
        CHECK(link(source, target) == 0);
    }
    for (int i = 0; i < 32; i++) {
        snprintf(symlink_path, sizeof(symlink_path), "var/tmp-copy/usr/bin/link-%d", i);
        CHECK(symlink("../lib", symlink_path) == 0);
    }
    return 0;
}

int main(int argc, char **argv) {
    CHECK(argc == 3);
    int n = atoi(argv[2]);
    CHECK(n > 0 && chdir(argv[1]) == 0);
    CHECK(mkdir("src", 0700) == 0);
    CHECK(mkdir("var", 0700) == 0);
    CHECK(mkdir("var/tmp-copy", 0700) == 0);
    CHECK(mkdir("var/tmp-copy/usr", 0700) == 0);
    CHECK(mkdir("var/tmp-copy/usr/lib", 0700) == 0);
    CHECK(mkdir("var/tmp-copy/usr/bin", 0700) == 0);
    for (int i = 0; i < n * 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "src/file-%d", i);
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600 | S_ISVTX);
        CHECK(fd >= 0 && write(fd, "runtime", 7) == 7 && close(fd) == 0);
    }

    CHECK(copy_links(n) == 0);
    puts("PHASE 1");
    fflush(stdout);
    sleep(2);
    for (int alias = 1; alias < 4; alias++) {
        for (int i = 0; i < n; i++) {
            char source[64], target[96];
            snprintf(source, sizeof(source), "src/file-%d", i);
            snprintf(target, sizeof(target), "var/tmp-copy/usr/lib/file-%d-%d", i, alias);
            CHECK(link(source, target) == 0);
        }
    }
    puts("PHASE 4");
    fflush(stdout);
    sleep(2);
    puts("PASS pressure-vessel-style hardlink runtime copy");
    return 0;
}
