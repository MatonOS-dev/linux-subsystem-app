/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
static void *worker(void *arg){puts("inprocess-worker");return arg;}
int main(int argc,char **argv) {
    if(argc<2 || strcmp(argv[1],"fixture") || !getenv("INPROCESS_TEST") || strcmp(getenv("INPROCESS_TEST"),"present"))return 20;
    if(access("/",F_OK))return 22;
    /* The inherited-filter host run also verifies unsupported raw retries. */
    errno=0;
    long priority=syscall(SYS_getpriority,0,0);
    if(priority==-1 && errno!=ENOSYS)return 23;
    printf("inprocess-payload pid=%d argv=%s env=%s\n",(int)getpid(),argv[1],getenv("INPROCESS_TEST"));
    pthread_t thread;void *answer;
    if(pthread_create(&thread,NULL,worker,(void *)42) || pthread_join(thread,&answer) || answer!=(void *)42)return 21;
    return argc>2 && !strcmp(argv[2],"status37")?37:0;
}
