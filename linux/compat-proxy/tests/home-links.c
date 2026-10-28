#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { perror(#x); return 1; } } while (0)
int main(int argc, char **argv) {
    CHECK(argc == 2 && chdir(argv[1]) == 0);
    CHECK(mkdir("runtime", 0700) == 0 && mkdir("prepared", 0700) == 0);
    int fd = open("runtime/library", O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && write(fd, "runtime", 7) == 7);
    CHECK(linkat(AT_FDCWD, "runtime/./library", AT_FDCWD,
                 "prepared/library", 0) == 0);
    struct stat a, b;
    CHECK(fstat(fd, &a) == 0 && stat("prepared/library", &b) == 0);
    CHECK(a.st_ino == b.st_ino && a.st_nlink == 2 && b.st_nlink == 2);
    CHECK(pwrite(fd, "R", 1, 0) == 1 && close(fd) == 0);
    fd = open("prepared/library", O_RDONLY);
    char data[8] = {0};
    CHECK(fd >= 0 && read(fd, data, 7) == 7 && !strcmp(data, "Runtime"));
    CHECK(close(fd) == 0 && unlink("runtime/library") == 0);
    CHECK(stat("prepared/library", &b) == 0 && b.st_nlink == 1);
    puts("PASS downloaded runtime hard links in stub HOME with kernel linkat denied");
    return 0;
}
