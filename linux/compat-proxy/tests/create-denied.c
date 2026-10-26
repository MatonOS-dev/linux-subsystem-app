#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    int fd=open("/denied/tmp.fixture",O_CREAT|O_EXCL|O_RDWR,0600);
    if(fd>=0){close(fd);return 1;}
    if(errno!=EACCES){perror("expected EACCES");return 1;}
    puts("PASS denied tempfile creation preserves EACCES");return 0;
}
