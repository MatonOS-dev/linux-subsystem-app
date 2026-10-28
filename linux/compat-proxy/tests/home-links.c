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
    CHECK(mkdir(".var", 0700) == 0 && mkdir(".var/app", 0700) == 0);
    CHECK(mkdir(".var/app/com.google.Chrome", 0700) == 0);
    CHECK(mkdir(".var/app/com.google.Chrome/config", 0700) == 0);
    CHECK(mkdir(".var/app/com.google.Chrome/config/google-chrome", 0700) == 0);
    CHECK(mkdir(".var/app/com.google.Chrome/config/google-chrome/Default", 0700) == 0);
    CHECK(mkdir(".var/app/com.google.Chrome/config/google-chrome/Default/Crashpad", 0700) == 0);
    CHECK(symlink("host-pid-cookie", ".var/app/com.google.Chrome/config/google-chrome/SingletonLock") == 0);
    CHECK(symlink("/tmp/chrome-singleton", ".var/app/com.google.Chrome/config/google-chrome/SingletonSocket") == 0);
    CHECK(symlink("host-pid-cookie", ".var/app/com.google.Chrome/config/google-chrome/SingletonCookie") == 0);
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

    /* Chrome profile databases and Crashpad files use hard links, alongside
     * its three symlink-based Singleton locks. Keep both forms working in the
     * same marked HOME tree. */
    fd = open(".var/app/com.google.Chrome/config/google-chrome/Default/Crashpad/report.db", O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && write(fd, "crashpad", 8) == 8 && close(fd) == 0);
    CHECK(linkat(AT_FDCWD, ".var/app/com.google.Chrome/config/google-chrome/Default/Crashpad/report.db",
                 AT_FDCWD, ".var/app/com.google.Chrome/config/google-chrome/Default/Crashpad/report.db.pending", 0) == 0);
    CHECK(symlink("report.db.pending", ".var/app/com.google.Chrome/config/google-chrome/Default/Crashpad/report.db.lock") == 0);
    char target[128] = {0};
    CHECK(readlink(".var/app/com.google.Chrome/config/google-chrome/SingletonLock", target, sizeof(target)) == 15);
    CHECK(!strcmp(target, "host-pid-cookie"));
    memset(target, 0, sizeof(target));
    CHECK(readlink(".var/app/com.google.Chrome/config/google-chrome/SingletonSocket", target, sizeof(target)) == 21);
    CHECK(!strcmp(target, "/tmp/chrome-singleton"));
    struct stat original, pending;
    CHECK(stat(".var/app/com.google.Chrome/config/google-chrome/Default/Crashpad/report.db", &original) == 0);
    CHECK(stat(".var/app/com.google.Chrome/config/google-chrome/Default/Crashpad/report.db.pending", &pending) == 0);
    CHECK(original.st_ino == pending.st_ino && original.st_nlink == 2);
    CHECK(unlink(".var/app/com.google.Chrome/config/google-chrome/Default/Crashpad/report.db") == 0);
    CHECK(rename(".var/app/com.google.Chrome/config/google-chrome/Default/Crashpad/report.db.pending",
                 ".var/app/com.google.Chrome/config/google-chrome/Default/Crashpad/report.db") == 0);
    fd = open(".var/app/com.google.Chrome/config/google-chrome/Default/Crashpad/report.db", O_RDONLY);
    memset(data, 0, sizeof(data));
    CHECK(fd >= 0 && read(fd, data, 8) == 8 && !memcmp(data, "crashpad", 8) && close(fd) == 0);
    puts("PASS downloaded runtime hard links in stub HOME with kernel linkat denied");
    return 0;
}
