#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc,char **argv) {
    int owned=atoi(getenv("RESIDENT_TEST_FD")),ordinary=atoi(getenv("PAYLOAD_TEST_FD"));
    assert(owned>2 && ordinary>2);
    assert(fcntl(owned,F_GETFD)>=0);
    if(argc==1) {
        assert(close(owned)==0 && fcntl(owned,F_GETFD)>=0);
        pid_t child=fork();assert(child>=0);
        if(!child) {
            assert(dup2(ordinary,owned)==owned);
            assert(fcntl(owned,F_GETFD)>=0);
            _exit(0);
        }
        int status=0;assert(waitpid(child,&status,0)==child);
        assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
        assert(syscall(SYS_close_range,3,UINT_MAX,4)==0);
        char *next[]={argv[0],"after-exec",NULL};execv(argv[0],next);abort();
    }
    assert(fcntl(ordinary,F_GETFD)==-1 && errno==EBADF);
    assert(syscall(SYS_close_range,3,UINT_MAX,0)==0);
    unsigned char byte=99;assert(read(owned,&byte,1)==1 && byte==0);
    puts("PASS adopted child may dup2 over resident fd; parent resident fd survives close/close_range/CLOEXEC exec");
    return 0;
}
