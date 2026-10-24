/* Runs as either ELF32 or ELF64, without either libc or a graphics device. */
#include "../timing.h"
int timing_test(void) {
    struct timespec first={0},last={0},invalid={0,-1},delay={0,1000000};
    int result=0;
    if(sys_clock_gettime(1,&first) || first.tv_sec<=0 || first.tv_nsec<0 || first.tv_nsec>=1000000000)result=1;
    if(sys_nanosleep(&invalid)!=-22)result=2;
    if(sys_nanosleep(&delay))result=3;
    if(sys_clock_gettime(1,&last) || last.tv_sec<first.tv_sec ||
       (last.tv_sec==first.tv_sec && last.tv_nsec<=first.tv_nsec))result=4;
    return result;
}
#if defined(__i386__)
__asm__(".globl _start\n_start: andl $-16,%esp; call timing_test; movl %eax,%ebx; movl $1,%eax; int $0x80; ud2");
#else
__asm__(".globl _start\n_start: andq $-16,%rsp; call timing_test; movl %eax,%edi; movl $60,%eax; syscall; ud2");
#endif
