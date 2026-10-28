/* Kernel-exec entry with the same inherited exemptions as the app tree. */
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
uint32_t proxy_bionic32_base;
uint64_t proxy_bionic64_base=UINT64_C(0x2000000000);
int main(int argc,char **argv){
 assert(argc>=2);
 char value[64];snprintf(value,sizeof(value),"%lu",(unsigned long)proxy_bionic64_base);
 assert(!setenv("MATON_V3_BIONIC64_BASE",value,1));
 const char *provider=getenv("MATON_TEST_GPU_PROVIDER");
 if(provider)assert(!setenv("LD_PRELOAD",provider,1));
 struct sock_filter code[48];unsigned n=bionic32_filter(code);
 if(getenv("MATON_TEST_NO_NATIVE_EXEMPT"))n=0;
 struct sock_filter rest[]={
  BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
  BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_I386,0,1),
  BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
  BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)+4),
  BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x6000,0,3),
  BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)),
  BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x300042,0,1),
  BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
  BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
 };
 /* Allow this harness's exec and rt_sigreturn; everything else outside the
  * native region/gateway traps after exec. */
 code[n++]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr));
 code[n++]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,59,2,0);
 code[n++]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,15,1,0);
 code[n++]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,157,0,1);
 code[n++]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
 for(unsigned i=0;i<sizeof(rest)/sizeof(rest[0]);i++)code[n++]=rest[i];
 struct sock_fprog filter={.len=n,.filter=code};
 assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));
 /* Model both an earlier routing filter and a payload filter: exemptions
  * must survive every inherited layer, not just the newest one. */
 assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&filter));
 assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&filter));
 execv(argv[1],argv+1);_exit(111);
}
