#define _GNU_SOURCE
#include <sys/mount.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#c,errno); return 1; } } while (0)

static ssize_t read_file(const char *path, char *buf, size_t size) {
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0)return -1;
    ssize_t n=read(fd,buf,size);
    close(fd);
    return n;
}

int main(void) {
    char before[4096],after[4096];
    ssize_t n=read_file("/usr/lib/os-release",before,sizeof(before));
    CHECK(n>0 && n<(ssize_t)sizeof(before));
    CHECK(chdir("/")==0);
    int oldroot_fd=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    CHECK(oldroot_fd>=0);
    CHECK(mount("tmpfs","/tmp","tmpfs",0,NULL)==0);
    CHECK(mkdir("/tmp/oldroot",0755)==0);
    CHECK(mkdir("/tmp/mnt",0755)==0);
    CHECK(syscall(SYS_pivot_root,"/tmp","/tmp/oldroot")==0);
    int source_fd=openat(oldroot_fd,"usr/lib/os-release",O_RDONLY|O_CLOEXEC);
    CHECK(source_fd>=0);
    ssize_t through_fd=read(source_fd,after,sizeof(after));
    close(source_fd);
    CHECK(through_fd==n && !memcmp(before,after,(size_t)n));
    CHECK(mount("usr/lib/os-release","/mnt/os-release",NULL,MS_BIND,NULL)==0);
    ssize_t m=read_file("/mnt/os-release",after,sizeof(after));
    CHECK(m==n && !memcmp(before,after,(size_t)n));
    puts("PASS oldroot bind source resolves through the pre-pivot /usr view");
    return 0;
}
