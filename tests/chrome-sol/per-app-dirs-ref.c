#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    int fd = open("/run/flatpak/per-app-dirs-ref", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open /run/flatpak/per-app-dirs-ref");
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != 0) {
        perror("invalid Flatpak per-app ref lock");
        return 2;
    }
    close(fd);
    puts("PASS bwrap execvp chrome reads the Flatpak per-app ref lock");
    return 0;
}
