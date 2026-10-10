/* i386 path of session-binder-filter.h, on the real kernel. A 64-bit parent
 * installs the filter and execs this freestanding 32-bit child, as Flatpak
 * multiarch apps (Steam) do. The old x86_64-only filter killed it (SIGSYS).
 *   cc -m32 -nostdlib -static -ffreestanding -fno-pic -O2 \
 *      -o i386-child session_binder_filter_i386_test.c
 *   cc -DPARENT -O2 -o i386-parent session_binder_filter_i386_test.c
 *   ./i386-parent ./i386-child && echo PASS */
#ifdef PARENT
#include "../session-binder-filter.h"
#include <stdio.h>
#include <unistd.h>
int main(int argc,char** argv) {
    if(argc<2){fprintf(stderr,"usage: %s CHILD\n",argv[0]);return 2;}
    if(session_filter_binder()){perror("install filter");return 1;}
    execv(argv[1],argv+1);perror("exec");return 1;
}
#else
static long sc3(long n,long a,long b,long c){long r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c):"memory");return r;}
void _start(void){
    int fail=0;
    if(sc3(54,-1,0xc0186201L,0)!=-1)  fail|=1;   /* ioctl BINDER_WRITE_READ(32) -> -EPERM */
    if(sc3(54,-1,0x40086200L,0)!=-9)  fail|=2;   /* ioctl DMA_BUF_IOCTL_SYNC -> -EBADF */
    if(sc3(20,0,0,0)<=0)              fail|=4;   /* getpid */
    sc3(1,fail,0,0);                             /* exit */
    for(;;){}
}
#endif
