#ifndef MATON_XWAYLAND_LIFETIME_H
#define MATON_XWAYLAND_LIFETIME_H
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
/* wlroots double-forks. Adopt its grandchild before creation, then pin the
 * executable-matched direct child with pidfd (never signal a reused PID). */
static int maton_xwayland_pin(const char *binary, pid_t *child) {
  DIR *tasks=opendir("/proc/self/task");if(!tasks)return -1;
  struct dirent *entry;int result=-1;
  while(result<0&&(entry=readdir(tasks))){
    if(entry->d_name[0]=='.')continue;
    char path[512];snprintf(path,sizeof(path),"/proc/self/task/%s/children",entry->d_name);
    FILE *list=fopen(path,"r");if(!list)continue;
    int pid;
    while(result<0&&fscanf(list,"%d",&pid)==1){
      int fd=(int)syscall(SYS_pidfd_open,pid,0);if(fd<0)continue;
      char exe[512];snprintf(path,sizeof(path),"/proc/%d/exe",pid);
      ssize_t n=readlink(path,exe,sizeof(exe)-1);
      if(n>=0){exe[n]=0;if(!strcmp(exe,binary)){*child=pid;result=fd;}}
      if(result<0)close(fd);
    }
    fclose(list);
  }
  closedir(tasks);return result;
}
static void maton_xwayland_end(int fd,pid_t child) {
  if(fd<0)return;
  /* Shutdown must finish before the stub exits, even if Xwayland is wedged. */
  if(syscall(SYS_pidfd_send_signal,fd,SIGKILL,NULL,0)==0){
    for(int i=0;i<100;i++){
      pid_t result=waitpid(child,NULL,WNOHANG);
      if(result==child||(result<0&&errno!=EINTR))break;
      usleep(1000);
    }
  }
  close(fd);
}
#endif
