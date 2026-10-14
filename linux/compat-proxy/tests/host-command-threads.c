#define _GNU_SOURCE
#include <pthread.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
static void *worker(void *arg) {
    long id=(long)arg;char a[128],b[128];
    snprintf(a,sizeof(a),"/app/cache-%ld.tmp",id);
    snprintf(b,sizeof(b),"/app/cache-%ld",id);
    for(int i=0;i<1000;i++) {
        int fd=(int)syscall(SYS_openat,AT_FDCWD,"/app/missing/file",O_CREAT|O_RDWR,0600);
        if(fd!=-1||errno!=ENOENT){perror("missing open");return (void *)1;}
        fd=(int)syscall(SYS_openat,AT_FDCWD,a,O_CREAT|O_RDWR,0600);
        if(fd<0){perror("open");return (void *)2;}
        if(flock(fd,LOCK_EX)||rename(a,b)||flock(fd,LOCK_UN)||close(fd)||unlink(b)){perror("sequence");return (void *)3;}
    }
    return NULL;
}
int main(void) {
    if(access("/.flatpak-info",F_OK))return 1;
    pthread_t t[8];
    for(long i=0;i<8;i++)if(pthread_create(t+i,NULL,worker,(void *)i))return 2;
    for(int i=0;i<8;i++){void *r;if(pthread_join(t[i],&r)||r)return 3;}
    puts("PASS resident HostCommand snapshot: 8000 worker openat/ENOENT/rename/flock sequences");
    return 0;
}
