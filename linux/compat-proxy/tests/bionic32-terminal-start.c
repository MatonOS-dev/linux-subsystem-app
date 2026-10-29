#include <errno.h>
/* SELinux-equivalent kernel rejection installed before the resident exists. */
#define main peer_start_main
#include "bionic32-start.c"
#undef main
int main(int argc,char **argv) {
 struct sock_filter code[]={
  BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
  BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_I386,0,4),
  BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
  BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,54,0,2),
  BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[1])),
  BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x5401,1,0),
  BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
  BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EACCES),
 };
 struct sock_fprog filter={sizeof(code)/sizeof(code[0]),code};
 assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));
 assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&filter));
 return peer_start_main(argc,argv);
}
