/* SPDX-License-Identifier: Apache-2.0 */
#include "routing.h"
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <stddef.h>
static void resident_fault_handler(int sig){(void)sig;}
int main(int argc,char **argv) {
    if(argc<2)return 2;
    char *env[]={"INPROCESS_TEST=present",NULL};
    pid_t pid=getpid();
    struct sigaction installed={0},after;
    installed.sa_handler=resident_fault_handler;
    sigemptyset(&installed.sa_mask);
    if(sigaction(SIGSEGV,&installed,NULL) || sigaction(SIGBUS,&installed,NULL))return 3;
    /* Model inherited zygote legacy-call traps; the JNI filter cannot undo
     * them. getpriority intentionally has no translation: it must return
     * ENOSYS at the gateway rather than recursively overflow SIGSYS. */
    if(getenv("INHERITED_FILTER")) {
        struct sock_filter code[]={
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_access,2,0),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getpriority,1,0),
            BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
            BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP)
        };
        struct sock_fprog program={sizeof(code)/sizeof(code[0]),code};
        if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) ||
           prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program))return 6;
    }
    int expected=getenv("EXPECTED_STATUS")?atoi(getenv("EXPECTED_STATUS")):0;
    for(int i=0;i<2;i++) {
        int status=inprocess_run(argv+1,env);
        printf("resident-alive pid=%d run=%d status=%d\n",(int)getpid(),i,status);
        if(status!=expected || getpid()!=pid)return 1;
        if(sigaction(SIGSEGV,NULL,&after) || after.sa_handler!=resident_fault_handler)return 4;
        if(sigaction(SIGBUS,NULL,&after) || after.sa_handler!=resident_fault_handler)return 5;
    }
    return 0;
}
