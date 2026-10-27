#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s: %s\n",__LINE__,#x,strerror(errno)); return 1; } } while (0)
int main(void) {
    int pipes[2], sockets[2];
    CHECK(pipe(pipes)==0);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    CHECK(epoll_create1(EPOLL_CLOEXEC)>=0);
    int deleted=memfd_create("proc-dirfd-deleted",MFD_CLOEXEC);
    CHECK(deleted>=0);
    char filename[]="/tmp/compat-proc-dirfd.XXXXXX";
    int regular=mkstemp(filename); CHECK(regular>=0);
    CHECK(unlink(filename)==0);
    int proc=open("/proc",O_RDONLY|O_DIRECTORY|O_CLOEXEC); CHECK(proc>=0);
    int task=openat(proc,"self/task/",O_RDONLY|O_DIRECTORY|O_CLOEXEC); CHECK(task>=0);
    struct stat st; CHECK(fstatat(task,".",&st,0)==0 && S_ISDIR(st.st_mode));
    DIR *tasks=fdopendir(task); CHECK(tasks);
    int saw_self=0; struct dirent *tid;
    while ((tid=readdir(tasks))) if (atoi(tid->d_name)==getpid()) saw_self=1;
    CHECK(saw_self); CHECK(closedir(tasks)==0);
    int fds=openat(proc,"self/fd/",O_RDONLY|O_DIRECTORY|O_CLOEXEC); CHECK(fds>=0);
    DIR *dir=fdopendir(fds); CHECK(dir);
    unsigned count=0; struct dirent *entry;
    /* Chromium HasOpenDirectory stats every entry, including pipe/socket/
     * anonymous/deleted objects whose readlink text is not a pathname. */
    while ((entry=readdir(dir))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        int fd=atoi(entry->d_name); struct stat expected;
        CHECK(fstat(fd,&expected)==0);
        CHECK(fstatat(fds,entry->d_name,&st,0)==0);
        CHECK(st.st_mode==expected.st_mode && st.st_ino==expected.st_ino && st.st_dev==expected.st_dev);
        CHECK(fstatat(fds,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISLNK(st.st_mode));
        count++;
    }
    CHECK(count>=9);
    char name[32]; snprintf(name,sizeof(name),"%d",deleted);
    int reopened=openat(fds,name,O_RDWR|O_CLOEXEC); CHECK(reopened>=0);
    CHECK(write(reopened,"x",1)==1);
    char byte; CHECK(pread(deleted,&byte,1,0)==1 && byte=='x');
    CHECK(closedir(dir)==0); CHECK(close(proc)==0);
    puts("PASS proc dirfd task/fd enumeration and magic-link object identity");
    return 0;
}
