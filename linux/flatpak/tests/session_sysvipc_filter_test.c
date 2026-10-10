/* session_filter_sysvipc on the real kernel: SysV calls reach the listener on
 * both ABIs, everything else runs natively. The parent forks, the child
 * installs both session filters, hands the listener over and execs a
 * freestanding child; the parent answers each notification with 1000 + nr.
 *   cc -nostdlib -static -ffreestanding -fno-pic -O2 -o sysv-child64 session_sysvipc_filter_test.c
 *   cc -m32 -nostdlib -static -ffreestanding -fno-pic -O2 -o sysv-child32 session_sysvipc_filter_test.c
 *   cc -DPARENT -O2 -o sysv-parent session_sysvipc_filter_test.c
 *   ./sysv-parent ./sysv-child64 && ./sysv-parent ./sysv-child32 && echo PASS */
#ifdef PARENT
#include "../session-binder-filter.h"
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
static void send_fd(int sock,int fd) {
    char byte=0;struct iovec io={&byte,1};
    union {struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int))];} control={0};
    struct msghdr msg={.msg_iov=&io,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    struct cmsghdr* cm=CMSG_FIRSTHDR(&msg);
    cm->cmsg_level=SOL_SOCKET;cm->cmsg_type=SCM_RIGHTS;cm->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm),&fd,sizeof(fd));
    if(sendmsg(sock,&msg,0)!=1)_exit(90);
}
static int recv_fd(int sock) {
    char byte;struct iovec io={&byte,1};
    union {struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int))];} control;
    struct msghdr msg={.msg_iov=&io,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    if(recvmsg(sock,&msg,MSG_CMSG_CLOEXEC)!=1)return -1;
    int fd;memcpy(&fd,CMSG_DATA(CMSG_FIRSTHDR(&msg)),sizeof(fd));return fd;
}
int main(int argc,char** argv) {
    if(argc<2){fprintf(stderr,"usage: %s CHILD\n",argv[0]);return 2;}
    int pair[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)){perror("socketpair");return 1;}
    pid_t child=fork();
    if(!child) {
        close(pair[0]);
        if(session_filter_binder())_exit(91);
        int listener=session_filter_sysvipc();
        if(listener<0)_exit(92);
        send_fd(pair[1],listener);close(listener);close(pair[1]);
        execv(argv[1],argv+1);_exit(93);
    }
    close(pair[1]);
    int listener=recv_fd(pair[0]);close(pair[0]);
    if(listener<0){fprintf(stderr,"no listener\n");return 1;}
    struct seccomp_notif_sizes sizes;
    if(syscall(SYS_seccomp,SECCOMP_GET_NOTIF_SIZES,0,&sizes)){perror("sizes");return 1;}
    struct seccomp_notif* notif=calloc(1,sizes.seccomp_notif);
    unsigned handled=0;
    for(;;) {
        struct pollfd p={.fd=listener,.events=POLLIN};
        if(poll(&p,1,5000)<=0||(p.revents&POLLHUP))break;
        memset(notif,0,sizes.seccomp_notif);
        if(ioctl(listener,SECCOMP_IOCTL_NOTIF_RECV,notif))continue;
        if(notif->data.arch!=AUDIT_ARCH_X86_64&&notif->data.arch!=AUDIT_ARCH_I386){fprintf(stderr,"bad arch\n");return 1;}
        struct seccomp_notif_resp resp={.id=notif->id,.val=1000+notif->data.nr};
        ioctl(listener,SECCOMP_IOCTL_NOTIF_SEND,&resp);handled++;
    }
    int status;waitpid(child,&status,0);
    if(!WIFEXITED(status)||WEXITSTATUS(status)){fprintf(stderr,"child status %d (handled %u)\n",status,handled);return 1;}
    printf("PASS: %s, %u SysV calls notified, other calls native\n",argv[1],handled);
    return 0;
}
#elif defined(__x86_64__)
static long sc(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
void _start(void){
    int fail=0;
    static const long nr[]={29,30,31,64,65,66,67,68,69,70,71,220};
    for(unsigned i=0;i<sizeof(nr)/sizeof(nr[0]);i++)if(sc(nr[i],0,0,0)!=1000+nr[i])fail|=1;
    if(sc(39,0,0,0)<=0)fail|=2;                 /* getpid stays native */
    if(sc(16,-1,0xc0306201L,0)!=-1)fail|=4;     /* Binder ioctl still -EPERM */
    sc(60,fail,0,0);
    for(;;){}
}
#else
static long sc3(long n,long a,long b,long c){long r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c):"memory");return r;}
void _start(void){
    int fail=0;
    static const long nr[]={117,393,394,395,396,397,398,399,400,401,402,420};
    for(unsigned i=0;i<sizeof(nr)/sizeof(nr[0]);i++)if(sc3(nr[i],0,0,0)!=1000+nr[i])fail|=1;
    if(sc3(20,0,0,0)<=0)fail|=2;                /* getpid stays native */
    if(sc3(54,-1,0xc0186201L,0)!=-1)fail|=4;    /* Binder ioctl still -EPERM */
    sc3(1,fail,0,0);
    for(;;){}
}
#endif
