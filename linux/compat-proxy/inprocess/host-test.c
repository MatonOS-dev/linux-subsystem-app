/* SPDX-License-Identifier: Apache-2.0 */
#include "routing.h"
#include <stdio.h>
#include <unistd.h>
int main(int argc,char **argv) {
    if(argc<2)return 2;
    char *env[]={"INPROCESS_TEST=present",NULL};
    pid_t pid=getpid();
    for(int i=0;i<2;i++) {
        int status=inprocess_run(argv+1,env);
        printf("resident-alive pid=%d run=%d status=%d\n",(int)getpid(),i,status);
        if(status || getpid()!=pid)return 1;
    }
    return 0;
}
