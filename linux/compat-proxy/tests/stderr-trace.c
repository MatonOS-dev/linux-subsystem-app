#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
int main(void) {
    int fd=open("/dev/null",O_WRONLY);assert(fd>=0);
    assert(dup2(fd,2)==2);close(fd);
    assert(write(2,"redirected-write",16)==16);
    struct iovec iov[]={{"redirected-",11},{"writev",6}};
    assert(writev(2,iov,2)==17);
    return 0;
}
