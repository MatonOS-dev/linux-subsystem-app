#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <spawn.h>
#include <sched.h>
extern char **environ;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#x,errno);return 1;}}while(0)
static int hidden(const char *path){DIR *d=opendir(path);if(!d)return -1;int n=0;struct dirent *e;while((e=readdir(d)))if(!strncmp(e->d_name,".matonos-tmp-",13))n++;closedir(d);return n;}
int main(int argc,char **argv){
 CHECK(argc>=2);char resolved[4096];CHECK(realpath(argv[1],resolved)!=NULL);argv[1]=resolved;
 if(argc==3 && !strcmp(argv[2],"--clean")){CHECK(hidden(argv[1])==0);return 0;}
 if(argc==5){int fd=atoi(argv[2]),sync=atoi(argv[3]);char c;CHECK(read(sync,&c,1)==1);close(sync);char proc[80];snprintf(proc,sizeof(proc),"/proc/%ld/fd/%d",(long)getpid(),fd);CHECK(linkat(AT_FDCWD,proc,AT_FDCWD,argv[4],AT_SYMLINK_FOLLOW)==0);close(fd);return 0;}
 CHECK(argc==2);CHECK(hidden(argv[1])==0);int dir=open(argv[1],O_DIRECTORY|O_RDONLY);CHECK(dir>=0);
 int fd=openat(dir,".",O_TMPFILE|O_RDWR,0640);CHECK(fd>=0);CHECK(hidden(argv[1])==1);CHECK(write(fd,"run-environ",11)==11);
 int alias=fcntl(fd,F_DUPFD_CLOEXEC,100);CHECK(alias>=0);close(fd);CHECK(hidden(argv[1])==1);int dupfd=dup(alias);CHECK(dupfd>=0);close(alias);CHECK(hidden(argv[1])==1);close(dupfd);CHECK(hidden(argv[1])==0);
 int original=openat(dir,".",O_TMPFILE|O_RDWR,0600),overwritten=openat(dir,".",O_TMPFILE|O_RDWR,0600);CHECK(original>=0 && overwritten>=0);CHECK(hidden(argv[1])==2);CHECK(dup3(original,overwritten,0)==overwritten);CHECK(hidden(argv[1])==1);close(original);CHECK(hidden(argv[1])==1);close(overwritten);CHECK(hidden(argv[1])==0);
 fd=openat(dir,".",O_TMPFILE|O_RDWR,0600);CHECK(fd>=0);CHECK(write(fd,"data",4)==4);struct stat tmpstat,pubstat;CHECK(fstat(fd,&tmpstat)==0 && tmpstat.st_nlink==1);
 int existing=openat(dir,"exists",O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(existing>=0);close(existing);
 errno=0;CHECK(linkat(fd,"",dir,"exists",AT_EMPTY_PATH)==-1 && errno==EEXIST);CHECK(hidden(argv[1])==1);
 CHECK(linkat(fd,"",dir,"published",AT_EMPTY_PATH)==0);CHECK(hidden(argv[1])==0);CHECK(fstatat(dir,"published",&pubstat,0)==0 && tmpstat.st_ino==pubstat.st_ino && tmpstat.st_dev==pubstat.st_dev);
 char procname[80],namedpath[4096],reported[4096];snprintf(procname,sizeof(procname),"/proc/self/fd/%d",fd);snprintf(namedpath,sizeof(namedpath),"%s/published",argv[1]);ssize_t length=readlink(procname,reported,sizeof(reported)-1);CHECK(length>0);reported[length]=0;CHECK(!strcmp(reported,namedpath));
 errno=0;CHECK(linkat(fd,"",dir,"second",AT_EMPTY_PATH)==-1 && errno==EPERM);close(fd);
 fd=openat(dir,".",O_TMPFILE|O_RDWR|O_EXCL,0600);CHECK(fd>=0);errno=0;CHECK(linkat(fd,"",dir,"exclusive",AT_EMPTY_PATH)==-1 && errno==EPERM);close(fd);CHECK(hidden(argv[1])==0);
 /* A child closes its copy, leaving the parent's last reference alive. */
 fd=openat(dir,".",O_TMPFILE|O_RDWR,0600);CHECK(fd>=0);pid_t child=fork();CHECK(child>=0);if(!child){close(fd);_exit(0);}int status;CHECK(waitpid(child,&status,0)==child && status==0);CHECK(hidden(argv[1])==1);close(fd);CHECK(hidden(argv[1])==0);
 /* Failed stack-based clone rolls back its reserved references. */
 fd=openat(dir,".",O_TMPFILE|O_RDWR,0600);CHECK(fd>=0);char stack[16384];errno=0;CHECK(syscall(SYS_clone,CLONE_SIGHAND|SIGCHLD,stack+sizeof(stack),NULL,NULL,0)==-1 && errno==EINVAL);close(fd);CHECK(hidden(argv[1])==0);
 /* posix_spawn uses a separate child stack; its exit releases inherited refs. */
 fd=openat(dir,".",O_TMPFILE|O_RDWR,0600);CHECK(fd>=0);char *spawnargs[]={"/bin/true",NULL};CHECK(posix_spawn(&child,spawnargs[0],NULL,NULL,spawnargs,environ)==0);CHECK(waitpid(child,&status,0)==child && status==0);CHECK(hidden(argv[1])==1);close(fd);CHECK(hidden(argv[1])==0);
 /* Parent closes first; inherited descriptor survives resident exec. */
 fd=openat(dir,".",O_TMPFILE|O_RDWR,0600);CHECK(fd>=0);int pipefd[2];CHECK(pipe(pipefd)==0);child=fork();CHECK(child>=0);
 if(!child){close(pipefd[1]);char f[32],p[32],target[4096];snprintf(f,sizeof(f),"%d",fd);snprintf(p,sizeof(p),"%d",pipefd[0]);snprintf(target,sizeof(target),"%s/inherited",argv[1]);execl(argv[0],argv[0],argv[1],f,p,target,NULL);_exit(8);}
 close(pipefd[0]);close(fd);CHECK(hidden(argv[1])==1);CHECK(write(pipefd[1],"x",1)==1);close(pipefd[1]);CHECK(waitpid(child,&status,0)==child && status==0);CHECK(hidden(argv[1])==0);
 /* Process exit also releases the last unpublished reference. */
 child=fork();CHECK(child>=0);if(!child){int t=openat(dir,".",O_TMPFILE|O_RDWR,0600);if(t<0)_exit(9);_exit(0);}CHECK(waitpid(child,&status,0)==child && status==0);CHECK(hidden(argv[1])==0);
 child=fork();CHECK(child>=0);if(!child){int t=openat(dir,".",O_TMPFILE|O_RDWR|O_CLOEXEC,0600);if(t<0)_exit(9);execl(argv[0],argv[0],argv[1],"--clean",NULL);_exit(10);}CHECK(waitpid(child,&status,0)==child && status==0);CHECK(hidden(argv[1])==0);
 CHECK(symlinkat("published",dir,"symlink")==0);errno=0;CHECK(linkat(dir,"symlink",dir,"symlink-copy",0)==-1 && errno==EPERM);
 CHECK(linkat(dir,"symlink",dir,"followed-copy",AT_SYMLINK_FOLLOW)==0);
 struct timespec times[2]={{123456789,123},{123456790,456}};CHECK(utimensat(dir,"published",times,0)==0);CHECK(fchmodat(dir,"published",0640,0)==0);
 CHECK(linkat(dir,"published",dir,"copy",0)==0);struct stat a,b;CHECK(fstatat(dir,"published",&a,0)==0 && fstatat(dir,"copy",&b,0)==0);CHECK(a.st_ino==b.st_ino && a.st_nlink==3 && b.st_nlink==3 && a.st_mode==b.st_mode && a.st_mtim.tv_sec==b.st_mtim.tv_sec && a.st_mtim.tv_nsec==b.st_mtim.tv_nsec && a.st_atim.tv_sec==b.st_atim.tv_sec && a.st_atim.tv_nsec==b.st_atim.tv_nsec);
 int copy=openat(dir,"copy",O_RDONLY);char buf[5]={0};CHECK(copy>=0 && read(copy,buf,4)==4 && !strcmp(buf,"data"));close(copy);
 errno=0;CHECK(linkat(dir,"published",dir,"copy",0)==-1 && errno==EEXIST);
 struct stat outside;CHECK(stat("/etc/hosts",&outside)==0);errno=0;CHECK(linkat(AT_FDCWD,"/etc/hosts",dir,"outside",0)==-1 && errno==(outside.st_dev==a.st_dev?EPERM:EXDEV));
 close(dir);puts("PASS denied-kernel linkat: hidden cleanup, NOREPLACE, dup, fork/exec, exit, shared named inode and metadata");return 0;
}
