/* SPDX-License-Identifier: Apache-2.0 */
#include "routing.h"
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
static void resident_fault_handler(int sig){(void)sig;}
int main(int argc,char **argv) {
    if(argc<2)return 2;
    char *env[]={"INPROCESS_TEST=present",NULL};
    pid_t pid=getpid();
    struct sigaction installed={0},after;
    installed.sa_handler=resident_fault_handler;
    sigemptyset(&installed.sa_mask);
    if(sigaction(SIGSEGV,&installed,NULL) || sigaction(SIGBUS,&installed,NULL))return 3;
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
