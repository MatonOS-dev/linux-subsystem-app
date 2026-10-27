#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>
#include <sys/socket.h>
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
        /* Zypak forwards every enumerated fd in one SCM_RIGHTS message; its
         * supervisor has room for 16. Resident EGL fds belong to the layer. */
        int pair[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
        DIR *dir=opendir("/proc/self/fd");assert(dir);
        int passed[253];size_t count=0;struct dirent *entry;
        while((entry=readdir(dir))) {
            char *end;long fd=strtol(entry->d_name,&end,10);
            if(*end || fd==dirfd(dir) || fd==pair[0] || fd==pair[1])continue;
            assert(count<253);passed[count++]=(int)fd;
        }
        closedir(dir);
        int saw_ordinary=0;
        for(size_t i=0;i<count;i++){assert(passed[i]!=owned);if(passed[i]==ordinary)saw_ordinary=1;}
        assert(saw_ordinary);
        char byte='z',control[CMSG_SPACE(sizeof(passed))]={0};
        struct iovec iov={&byte,1};
        struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,
                           .msg_controllen=CMSG_SPACE(count*sizeof(int))};
        struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;
        c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(count*sizeof(int));
        memcpy(CMSG_DATA(c),passed,count*sizeof(int));
        assert(sendmsg(pair[0],&msg,0)==1);
        char received[CMSG_SPACE(16*sizeof(int))]={0};
        msg.msg_control=received;msg.msg_controllen=sizeof(received);
        assert(recvmsg(pair[1],&msg,0)==1);
        assert(!(msg.msg_flags&(MSG_CTRUNC|MSG_TRUNC)));
        c=CMSG_FIRSTHDR(&msg);assert(c && c->cmsg_len==CMSG_LEN(count*sizeof(int)));
        for(size_t i=0;i<count;i++)close(((int *)CMSG_DATA(c))[i]);
        close(pair[0]);close(pair[1]);
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
    puts("PASS Zypak enumerate/SCM_RIGHTS excludes 125 resident fds; adopted child may dup2 over resident fd; parent resident fd survives close/close_range/CLOEXEC exec");
    return 0;
}
