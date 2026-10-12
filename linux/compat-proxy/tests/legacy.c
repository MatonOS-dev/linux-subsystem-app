#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#x,errno);return 1;}}while(0)
int main(int argc,char **argv){
 CHECK(argc==2);CHECK(chdir(argv[1])==0);
 CHECK(syscall(SYS_mkdir,"legacy",0700)==0);
 int fd=(int)syscall(SYS_creat,"legacy/a",0600);CHECK(fd>=0);
 CHECK(syscall(SYS_dup2,fd,fd)==fd);errno=0;CHECK(syscall(SYS_dup2,-1,-1)==-1 && errno==EBADF);
 int copy=(int)syscall(SYS_dup2,fd,80);CHECK(copy==80);close(copy);close(fd);
 struct stat st;CHECK(syscall(SYS_lstat,"legacy/a",&st)==0);
 CHECK(syscall(SYS_access,"legacy/a",R_OK)==0);
 CHECK(syscall(SYS_chmod,"legacy/a",0640)==0);
 CHECK(syscall(SYS_link,"legacy/a","legacy/b")==0);
 CHECK(syscall(SYS_rename,"legacy/b","legacy/c")==0);
 CHECK(syscall(SYS_symlink,"a","legacy/s")==0);
 char buf[256];CHECK(syscall(SYS_readlink,"legacy/s",buf,sizeof(buf))==1 && buf[0]=='a');
 long times[2]={100,200};CHECK(syscall(SYS_utime,"legacy/a",times)==0);
 struct timeval tv[2]={{300,1},{400,2}};CHECK(syscall(SYS_utimes,"legacy/a",tv)==0);
 int pipes[2];CHECK(syscall(SYS_pipe,pipes)==0);CHECK(write(pipes[1],"x",1)==1);
 struct pollfd pollfd={.fd=pipes[0],.events=POLLIN};CHECK(syscall(SYS_poll,&pollfd,1,0)==1);
 fd_set fds;FD_ZERO(&fds);FD_SET(pipes[0],&fds);struct timeval timeout={0};CHECK(syscall(SYS_select,pipes[0]+1,&fds,NULL,NULL,&timeout)==1);
 close(pipes[0]);close(pipes[1]);
 CHECK(syscall(SYS_getpgrp)==getpgid(0));CHECK(syscall(SYS_time,NULL)>0);CHECK(syscall(SYS_alarm,0)>=0);
 fd=(int)syscall(SYS_epoll_create,1);CHECK(fd>=0);struct epoll_event event;CHECK(syscall(SYS_epoll_wait,fd,&event,1,0)==0);close(fd);
 fd=(int)syscall(SYS_inotify_init);CHECK(fd>=0);close(fd);fd=(int)syscall(SYS_eventfd,0);CHECK(fd>=0);close(fd);
 fd=open("legacy",O_DIRECTORY);CHECK(fd>=0);CHECK(syscall(SYS_getdents,fd,buf,sizeof(buf))>0);close(fd);
 CHECK(syscall(SYS_unlink,"legacy/a")==0);CHECK(syscall(SYS_unlink,"legacy/c")==0);CHECK(syscall(SYS_unlink,"legacy/s")==0);CHECK(syscall(SYS_rmdir,"legacy")==0);
 pid_t child=(pid_t)syscall(SYS_fork);CHECK(child>=0);if(!child)_exit(23);int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==23);
 puts("PASS legacy path/fd/poll/select/event/time/directory translations");return 0;
}
