#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
uint32_t proxy_bionic32_base;
static volatile sig_atomic_t traps;
static void trap(int sig,siginfo_t *info,void *context){
 (void)sig;assert(info->si_syscall==20);traps++;
 ((ucontext_t *)context)->uc_mcontext.gregs[REG_EAX]=12345;
}
int main(int argc,char **argv,char **env){
 (void)argc;(void)argv;(void)env;
 assert(proxy_bionic32_base);
 /* Real bionic calls (including TLS, allocator, stdio and fork startup) must
  * never hit the inherited TRAP filters: handler isn't installed yet. */
 assert(getpid()>0);void *allocation=malloc(10000);assert(allocation);free(allocation);
 FILE *maps=fopen("/proc/self/maps","r");assert(maps);
 char *line=NULL;size_t size=0;unsigned native=0;
 while(getline(&line,&size,maps)>0){
  assert(!strstr(line,"[anon:matonos-bionic32-startup]"));
  if(strstr(line,"libc.so") || strstr(line,"/bin/linker") || strstr(line,"libbionic32-peer.so")){
   unsigned lo,hi;assert(sscanf(line,"%x-%x",&lo,&hi)==2);
   assert(lo>=proxy_bionic32_base && hi<=proxy_bionic32_base+0x10000000);native++;
  }
 }
 assert(native>=3);free(line);fclose(maps);
 struct sigaction action={.sa_sigaction=trap,.sa_flags=SA_SIGINFO};
 assert(!sigaction(SIGSYS,&action,NULL));
 /* Place a genuine payload syscall outside the native reservation. */
 unsigned char *site=mmap((void *)0x09000000,4096,3,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 assert(site==(void *)0x09000000);
 const unsigned char code[]={0xb8,20,0,0,0,0xcd,0x80,0xc3};memcpy(site,code,sizeof(code));
 assert(!mprotect(site,4096,5));
 int (*volatile invoke)(void)=(void *)site;
 assert(invoke()==12345 && traps==1);
 pid_t child=fork();assert(child>=0);
 if(!child){assert(getpid()>0);_exit(0);}
 int status;assert(waitpid(child,&status,0)==child && status==0);
 puts("PASS kernel ELF64 -> ELF32 bionic startup: libc native, payload trapped, fork works");fflush(stdout);return 0;
}
