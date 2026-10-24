#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__x86_64__)
#define FIXED_CALL(name, number) \
__attribute__((noinline)) static long name(long a,long b,long c,long d,long e) { \
    register long r10 __asm__("r10")=d; register long r8 __asm__("r8")=e; long out; \
    __asm__ volatile("mov $" #number ",%%eax; syscall" : "=a"(out) \
       : "D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8) : "rcx","r11","memory"); \
    return out; \
}
FIXED_CALL(bench_openat,257)
FIXED_CALL(bench_stat,262)
FIXED_CALL(bench_statx,332)
FIXED_CALL(bench_close,3)
static long checked(long r){if(r<0 && r>=-4095){errno=(int)-r;return -1;}return r;}
#else
#define bench_openat(a,b,c,d,e) syscall(SYS_openat,a,b,c,d)
#define bench_stat(a,b,c,d,e) syscall(SYS_newfstatat,a,b,c,d)
#define bench_statx(a,b,c,d,e) syscall(SYS_statx,a,b,c,d,e)
#define bench_close(a,b,c,d,e) syscall(SYS_close,a)
#define checked(r) (r)
#endif

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s MODE ITERATIONS PATH\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1], *path = argv[3];
    char *end = NULL;
    unsigned long iterations = strtoul(argv[2], &end, 10);
    if (!iterations || !end || *end) return 2;

    volatile long checksum = 0;
    struct stat st;
    uint64_t start = now_ns();
    if (!strcmp(mode, "read")) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { perror("open read"); return 1; }
        size_t remaining = iterations;
        char buffer[65536];
        while (remaining) {
            size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            ssize_t n = read(fd, buffer, want);
            if (n < 0) { perror("read"); return 1; }
            if (!n) break;
            for (ssize_t i = 0; i < n; i += 4096) checksum += (unsigned char)buffer[i];
            remaining -= (size_t)n;
        }
        if (remaining) { fprintf(stderr, "short read: %zu bytes remain\n", remaining); return 1; }
        close(fd);
    } else if (!strcmp(mode, "mmap")) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { perror("open mmap"); return 1; }
        void *map = mmap(NULL, iterations, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) { perror("mmap"); return 1; }
        volatile const unsigned char *bytes = map;
        for (size_t i = 0; i < iterations; i += 4096) checksum += bytes[i];
        munmap(map, iterations);
        close(fd);
    } else for (unsigned long i = 0; i < iterations; ++i) {
        if (!strcmp(mode, "getpid")) {
            checksum += syscall(SYS_getpid);
        } else if (!strcmp(mode, "noop")) {
            /* Syscall 1000 is unknown to the x86_64 decoder. The proxy returns
             * -ENOSYS without entering the kernel, isolating SIGSYS delivery,
             * handler dispatch and return. */
            errno = 0;
            long r = syscall(1000);
            if (r != -1 || errno != ENOSYS) {
                fprintf(stderr, "unknown-syscall no-op returned %ld errno=%d\n", r, errno);
                return 1;
            }
            checksum += errno;
        } else if (!strcmp(mode, "stat")) {
            long r = checked(bench_stat(AT_FDCWD, (long)path, (long)&st, 0, 0));
            if (r) { perror("newfstatat"); return 1; }
            checksum += st.st_size;
        } else if (!strcmp(mode, "statx")) {
            struct statx sx;
            long r = checked(bench_statx(AT_FDCWD, (long)path, 0, STATX_BASIC_STATS, (long)&sx));
            if (r) { perror("statx"); return 1; }
            checksum += sx.stx_size;
        } else if (!strcmp(mode, "open")) {
            long fd = checked(bench_openat(AT_FDCWD, (long)path, O_RDONLY | O_CLOEXEC, 0, 0));
            if (fd < 0) { perror("openat"); return 1; }
            checksum += checked(bench_close(fd,0,0,0,0));
        } else {
            fprintf(stderr, "unknown mode: %s\n", mode);
            return 2;
        }
    }
    uint64_t elapsed = now_ns() - start;
    printf("mode=%s iterations=%lu elapsed_ns=%llu ns_per_op=%.2f checksum=%ld\n",
           mode, iterations, (unsigned long long)elapsed,
           (double)elapsed / (double)iterations, checksum);
    return 0;
}
