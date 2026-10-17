#define _GNU_SOURCE
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#x,errno);return 1;}}while(0)
int main(int argc,char **argv){
 CHECK(argc==2);int dir=open(argv[1],O_DIRECTORY|O_RDONLY);CHECK(dir>=0);
 errno=0;CHECK(openat(dir,".",O_TMPFILE|O_RDWR,0600)==-1 && errno==EOPNOTSUPP);
 int fd=openat(dir,"named",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
 CHECK(write(fd,"data",4)==4);char proc[80];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fd);
 errno=0;CHECK(linkat(AT_FDCWD,proc,dir,"published",AT_SYMLINK_FOLLOW)==-1 && (errno==EPERM || errno==EXDEV || errno==EACCES));
 close(fd);close(dir);puts("PASS O_TMPFILE fallback and non-store proc-fd link refused");return 0;
}
