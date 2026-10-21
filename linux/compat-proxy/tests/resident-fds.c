#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc,char **argv) {
    int owned=atoi(getenv("RESIDENT_TEST_FD")),ordinary=atoi(getenv("PAYLOAD_TEST_FD"));
    assert(owned>2 && ordinary>2);
    assert(fcntl(owned,F_GETFD)>=0);
    if(argc==1) {
        assert(close(owned)==0 && fcntl(owned,F_GETFD)>=0);
        assert(dup2(ordinary,owned)==-1 && errno==EBADF);
        assert(syscall(SYS_close_range,3,UINT_MAX,4)==0);
        char *next[]={argv[0],"after-exec",NULL};execv(argv[0],next);abort();
    }
    assert(fcntl(ordinary,F_GETFD)==-1 && errno==EBADF);
    assert(syscall(SYS_close_range,3,UINT_MAX,0)==0);
    unsigned char byte=99;assert(read(owned,&byte,1)==1 && byte==0);
    puts("PASS resident Android fd survives Linux close/dup2/close_range/CLOEXEC exec; payload fd closes");
    return 0;
}
