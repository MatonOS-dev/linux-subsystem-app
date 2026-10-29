#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
#define OK(x) do { if ((x) < 0) { perror(#x); exit(1); } } while (0)
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    OK(chdir(argv[1]));
    const int n = 3000;
    char a[64], b[64];
    double t = now();
    for (int i = 0; i < n; i++) {
        snprintf(a, sizeof(a), "source-%d", i);
        int fd = open(a, O_CREAT | O_EXCL | O_RDWR, 0600);
        OK(fd); OK(close(fd));
    }
    printf("create %.2f us/op\n", (now() - t) * 1e6 / n);
    t = now();
    for (int i = 0; i < n; i++) {
        snprintf(a, sizeof(a), "source-%d", i);
        snprintf(b, sizeof(b), "dest-%d", i);
        OK(link(a, b));
    }
    printf("link %.2f us/op\n", (now() - t) * 1e6 / n);
    t = now();
    for (int i = 0; i < n; i++) {
        snprintf(b, sizeof(b), "dest-%d", i);
        int fd = open(b, O_RDONLY);
        OK(fd); OK(close(fd));
    }
    printf("open %.2f us/op\n", (now() - t) * 1e6 / n);
    struct stat st;
    OK(stat("source-0", &st));
    if (st.st_nlink != 2) return 2;
    t = now();
    for (int i = 0; i < n; i++) {
        snprintf(b, sizeof(b), "dest-%d", i);
        OK(unlink(b));
    }
    printf("unlink %.2f us/op\n", (now() - t) * 1e6 / n);
    OK(stat("source-0", &st));
    return st.st_nlink != 1;
}
