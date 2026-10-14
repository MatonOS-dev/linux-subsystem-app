/* SPDX-License-Identifier: Apache-2.0
 * SSE/x87 state must survive SIGSYS and nested application signal frames. */
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
static volatile int caught;
static void nested(int sig){caught+=sig;__asm__ volatile("pxor %%xmm7,%%xmm7; fldz; fstp %%st(0)" ::: "xmm7");}
int main(int argc,char **argv,char **env){
 (void)argc;(void)argv;(void)env;
 static const uint32_t expected[4]={0x11223344,0x55667788,0x99aabbcc,0xddeeff00};uint32_t actual[4];
 /* access is trapped under both policies; every handler C scratch register is
  * restored by the kernel's original FP frame, including the x87 stack. */
 long result;const char *path="/proc/self/status";
 __asm__ volatile("movdqu %2,%%xmm7; fld1; int $0x80; movdqu %%xmm7,%1"
  :"=a"(result),"=m"(actual):"m"(expected),"0"(33),"b"(path),"c"(0):"memory","xmm7");
 unsigned char value[10],one[10];__asm__ volatile("fstpt %0; fld1; fstpt %1":"=m"(value),"=m"(one));
 if(result || memcmp(actual,expected,sizeof(actual)) || memcmp(value,one,sizeof(value)))return 71;
 signal(10,nested);
 __asm__ volatile("movdqu %0,%%xmm7; fld1"::"m"(expected):"xmm7");
 if(raise(10))return 72;
 __asm__ volatile("movdqu %%xmm7,%0; fstpt %1":"=m"(actual),"=m"(value));
 if(caught!=10 || memcmp(actual,expected,sizeof(actual)) || memcmp(value,one,sizeof(value)))return 73;
 static const char ok[]="PASS i386 signal-frame SSE and x87 restoration\n";
 return write(1,ok,sizeof(ok)-1)==sizeof(ok)-1?0:74;
}
