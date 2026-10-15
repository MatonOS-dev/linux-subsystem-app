#define _GNU_SOURCE
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#ifndef REFUSAL
#define REFUSAL EACCES
#endif
/* Both ABI syscall numbers, even when a resident exec switches class. */
int main(int argc,char **argv){
 if(argc<2)return 2;
 struct sock_filter rules[]={
 BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
 BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,9,1,0),
 BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,192,0,5),
 BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[2])),
 BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K,PROT_EXEC,0,3),
 BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[3])),
 BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K,MAP_ANONYMOUS,1,0),
 BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|REFUSAL),
 BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
 struct sock_fprog p={sizeof(rules)/sizeof(rules[0]),rules};
 if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)||prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&p))return 3;
 execv(argv[1],argv+1);return 4;
}
