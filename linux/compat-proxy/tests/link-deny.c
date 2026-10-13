#define _GNU_SOURCE
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
int main(int argc,char **argv){
 if(argc<2)return 2;
 struct sock_filter rules[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_linkat,0,1),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EACCES),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
 struct sock_fprog p={sizeof(rules)/sizeof(rules[0]),rules};
 if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) || prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&p))return 3;
 /* Prove this kernel filter denies linkat before the compatibility layer. */
 errno=0;if(syscall(SYS_linkat,AT_FDCWD,"missing",AT_FDCWD,"missing2",0)!=-1 || errno!=EACCES)return 4;
 execv(argv[1],argv+1);return 5;
}
