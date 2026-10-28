#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s: %s\n",__LINE__,#x,strerror(errno)); return 1; } } while (0)
int main(int argc,char **argv) {
    CHECK(argc==2);
    CHECK(mkdir(argv[1],0700)==0);
    int source=open(argv[1],O_PATH|O_DIRECTORY|O_CLOEXEC);
    CHECK(source>=0);
    CHECK(symlinkat("libfixture.so.1",source,"libfixture.so")==0);
    char proc[64];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",source);
    int dir=open(proc,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    CHECK(dir>=0 && close(source)==0);
    char target[64]={0};
    CHECK(readlinkat(dir,"libfixture.so",target,sizeof(target))==15);
    CHECK(!strcmp(target,"libfixture.so.1"));
    struct stat st;
    CHECK(fstatat(dir,"libfixture.so",&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISLNK(st.st_mode));
    /* Reusing the old number must not redirect the surviving descriptor. */
    int other=open("/",O_PATH|O_DIRECTORY|O_CLOEXEC);
    CHECK(other>=0);
    memset(target,0,sizeof(target));
    CHECK(readlinkat(dir,"libfixture.so",target,sizeof(target))==15);
    CHECK(!strcmp(target,"libfixture.so.1"));
    CHECK(close(other)==0 && unlinkat(dir,"libfixture.so",0)==0 && close(dir)==0);
    CHECK(rmdir(argv[1])==0);
    puts("PASS reopened proc dirfd survives source close and fd reuse");
    return 0;
}
