/* Host-side kernel exec into the actual bionic ELF32 startup under both
 * inherited routing and payload filters. No ptrace and no VM. */
#define _GNU_SOURCE
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include "../bionic32.h"
uint32_t proxy_bionic32_base=0x40000000;
int main(int argc,char **argv){
 assert(argc>=2);
 if(argc>2)proxy_bionic32_base=(uint32_t)strtoul(argv[2],NULL,0);
 char value[64];snprintf(value,sizeof(value),"%u",proxy_bionic32_base);
 assert(!setenv("MATON_V3_BIONIC32_BASE",value,1));
 const char *provider=getenv("MATON_TEST_GPU_PROVIDER");
 if(provider)assert(!setenv("LD_PRELOAD",provider,1));
 struct sock_filter code[32];unsigned n=bionic32_filter(code);
 if(getenv("MATON_TEST_NO_NATIVE_EXEMPT"))n=0;
 struct sock_filter rest[]={
  BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
  BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_I386,0,10),
  BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
  BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,173,0,1),
  BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
  BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)+4),
  BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0,0,4),
  BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)),
  BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x70300042,0,2),
  BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
  BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
  BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
  BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
 };
 for(unsigned i=0;i<sizeof(rest)/sizeof(rest[0]);i++)code[n++]=rest[i];
 struct sock_fprog filter={.len=n,.filter=code};
 assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));
 /* Model an earlier resident routing filter and an inherited Flatpak policy;
  * both must exempt internal startup, not just the newest filter. */
 assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&filter));
 assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&filter));
 if(argc>3)execv(argv[1],argv+3);
 else execl(argv[1],argv[1],NULL);
 perror("peer exec");return 1;
}
