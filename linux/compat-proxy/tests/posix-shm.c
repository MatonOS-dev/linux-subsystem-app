#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { perror(#x); return 1; } } while (0)

int main(void) {
    char name[80];
    snprintf(name, sizeof(name), "/matonos-shm-%ld", (long)getpid());

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0);
    errno = 0;
    CHECK(shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600) == -1 && errno == EEXIST);
    CHECK(ftruncate(fd, 4096) == 0);
    char *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(shared != MAP_FAILED);
    strcpy(shared, "parent");

    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        strcpy(shared, "child-visible");
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(strcmp(shared, "child-visible") == 0);
    CHECK(shm_unlink(name) == 0);
    errno = 0;
    CHECK(shm_open(name, O_RDONLY, 0) == -1 && errno == ENOENT);
    CHECK(strcmp(shared, "child-visible") == 0);
    CHECK(munmap(shared, 4096) == 0);
    CHECK(close(fd) == 0);
    puts("PASS POSIX shm O_EXCL, unlink, and shared mmap across processes");
    return 0;
}
