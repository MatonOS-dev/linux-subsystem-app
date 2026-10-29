#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc,char **argv){
 assert(argc==2);
 char file[4096],link[4096];
 assert(snprintf(file,sizeof(file),"%s/new",argv[1])>0);
 assert(snprintf(link,sizeof(link),"%s/link",argv[1])>0);
 struct stat st;char buf[4096];
 for(int i=0;i<10;i++){
  errno=0;assert(stat(file,&st)==-1 && errno==ENOENT);
  errno=0;assert(stat(link,&st)==-1 && errno==ENOENT);
  errno=0;assert(readlink(link,buf,sizeof(buf))==-1 && errno==ENOENT);
 }
 /* The parent's cache is unchanged by mutations in this forked process. */
 pid_t pid=fork();assert(pid>=0);
 if(!pid){int fd=open(file,O_CREAT|O_EXCL|O_WRONLY,0600);assert(fd>=0);assert(write(fd,"x",1)==1);assert(!close(fd));assert(!symlink(file,link));_exit(0);}
 int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
 assert(!stat(file,&st) && st.st_size==1);
 assert(readlink(link,buf,sizeof(buf))==(ssize_t)strlen(file));
 assert(!memcmp(buf,file,strlen(file)));
 assert(!stat(link,&st) && st.st_size==1);
 assert(!unlink(link) && !unlink(file));
 puts("PASS negative translations recheck files and symlinks created by another process");
 return 0;
}
