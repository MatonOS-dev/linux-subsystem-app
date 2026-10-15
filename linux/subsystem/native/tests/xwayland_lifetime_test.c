#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include "../xwayland_lifetime.h"
int main(void) {
  assert(prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0)==0);
  int ready[2];assert(pipe(ready)==0);
  pid_t helper=fork();assert(helper>=0);
  if(!helper){pid_t grandchild=fork();assert(grandchild>=0);
    if(!grandchild){close(ready[0]);execl("/bin/sleep","sleep","60",NULL);_exit(127);}
    write(ready[1],&grandchild,sizeof(grandchild));_exit(0);
  }
  close(ready[1]);pid_t actual;assert(read(ready[0],&actual,sizeof(actual))==sizeof(actual));close(ready[0]);
  assert(waitpid(helper,NULL,0)==helper);
  char binary[512],path[64];snprintf(path,sizeof(path),"/proc/%d/exe",actual);
  int fd=-1;pid_t tracked=0;
  for(int i=0;i<100&&fd<0;i++){
    ssize_t n=readlink(path,binary,sizeof(binary)-1);assert(n>0);binary[n]=0;
    /* Wait until exec has replaced the test binary. */
    if(strstr(binary,"sleep"))fd=maton_xwayland_pin(binary,&tracked);
    if(fd<0)usleep(1000);
  }
  assert(fd>=0&&tracked==actual);maton_xwayland_end(fd,tracked);
  assert(waitpid(actual,NULL,WNOHANG)==-1&&errno==ECHILD);
  assert(maton_xwayland_pin(binary,&tracked)==-1);
  puts("PASS double-fork adoption, pidfd tracking, termination and reaping");
}
