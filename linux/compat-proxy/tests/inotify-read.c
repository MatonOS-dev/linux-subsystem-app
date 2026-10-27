#define _GNU_SOURCE
#include "../inotify-read.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

static int is_inotify(int fd) {
    char path[64],target[64];
    int length=snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
    if(length<0 || (size_t)length>=sizeof(path))return 0;
    ssize_t n=readlink(path,target,sizeof(target)-1);
    if(n<0)return 0;
    target[n]=0;
    return !strcmp(target,"anon_inode:inotify");
}

int main(void) {
    int notify=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);
    assert(notify>=0 && is_inotify(notify));
    assert(proxy_inotify_read_result(256,0,is_inotify(notify))==-EAGAIN);
    assert(proxy_inotify_read_result(256,8,is_inotify(notify))==8);
    assert(proxy_inotify_read_result(0,0,is_inotify(notify))==0);

    int pipefd[2];assert(pipe(pipefd)==0);
    assert(!is_inotify(pipefd[0]));
    assert(proxy_inotify_read_result(256,0,is_inotify(pipefd[0]))==0);
    close(pipefd[0]);close(pipefd[1]);close(notify);
    puts("PASS positive-length inotify zero reads become EAGAIN; ordinary EOF is preserved");
    return 0;
}
