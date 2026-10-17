#define _GNU_SOURCE
#include <pthread.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#define N 8
#define ROUNDS 1000
static pthread_barrier_t barrier;
static pthread_mutex_t publish=PTHREAD_MUTEX_INITIALIZER;
static int fds[N],repo,serialized,keep_errors,failures;
static long tids[N];
static void check(int ok,const char *what){if(!ok){perror(what);exit(1);}}
static void directory(const char *path){check(mkdir(path,0700)==0 || errno==EEXIST,path);}
static void *worker(void *arg){
 long id=(long)arg;
 tids[id]=syscall(SYS_gettid);
 pthread_barrier_wait(&barrier);
 for(int round=0;round<ROUNDS;round++){
  fds[id]=open("/tmpfile-test/installation/repo/tmp",O_TMPFILE|O_RDWR,0600);
  check(fds[id]>=0,"O_TMPFILE");
  long payload=round*N+id;
  check(write(fds[id],&payload,sizeof(payload))==sizeof(payload),"write tmpfile");
  pthread_barrier_wait(&barrier);
  int owner=(round&1)?(id+1)%N:id;
  char source[128],dest[128];
  switch((round/2)%4){
   case 0:snprintf(source,sizeof(source),"/proc/self/fd/%d",fds[owner]);break;
   case 1:snprintf(source,sizeof(source),"/proc/%d/fd/%d",getpid(),fds[owner]);break;
   case 2:snprintf(source,sizeof(source),"/proc/thread-self/fd/%d",fds[owner]);break;
   default:snprintf(source,sizeof(source),"/proc/%ld/fd/%d",tids[owner],fds[owner]);break;
  }
  snprintf(dest,sizeof(dest),"objects/%ld-%d",id,round);
  /* Diagnostic mode isolates open-vs-close from simultaneous publications.
   * TMPFILE_KEEP_ERRORS records later failures too when tracing the baseline. */
  if(serialized)pthread_mutex_lock(&publish);
  if(linkat(AT_FDCWD,source,repo,dest,AT_SYMLINK_FOLLOW)){
   fprintf(stderr,"round=%d thread=%ld owner=%d source=%s errno=%d\n",round,id,owner,source,errno);
   if(!keep_errors)exit(1);
   __atomic_add_fetch(&failures,1,__ATOMIC_RELAXED);
  }else{
   struct stat a,b;
   check(fstat(fds[owner],&a)==0 && fstatat(repo,dest,&b,AT_SYMLINK_NOFOLLOW)==0,"published stat");
   check(S_ISREG(b.st_mode) && a.st_dev==b.st_dev && a.st_ino==b.st_ino && b.st_nlink==1,"published identity");
   int fd=openat(repo,dest,O_RDONLY|O_NOFOLLOW);
   long got=-1;
   check(fd>=0 && read(fd,&got,sizeof(got))==sizeof(got) && got==round*N+owner,"published contents");
   check(close(fd)==0 && unlinkat(repo,dest,0)==0,"published cleanup");
  }
  if(serialized)pthread_mutex_unlock(&publish);
  pthread_barrier_wait(&barrier);
  if(serialized)pthread_mutex_lock(&publish);
  check(close(fds[id])==0,"close tmpfile");
  if(serialized)pthread_mutex_unlock(&publish);
  /* No barrier here: the next opens must race the previous round's closes. */
 }
 return NULL;
}
static void foreign_process(void){
 int send[2],ack[2];
 check(pipe(send)==0 && pipe(ack)==0,"pipes");
 int unrelated=open("/tmpfile-test/installation/repo/tmp",O_TMPFILE|O_RDWR,0600);
 check(unrelated>=0,"unrelated tmpfile");
 pid_t child=fork();check(child>=0,"fork");
 if(!child){
  close(unrelated);
  int fd=open("/tmpfile-test/installation/repo/tmp",O_TMPFILE|O_RDWR,0600);
  check(fd>=0 && write(send[1],&fd,sizeof(fd))==sizeof(fd),"child tmpfile");
  char done;check(read(ack[0],&done,1)==1,"ack");close(fd);_exit(0);
 }
 int fd;check(read(send[0],&fd,sizeof(fd))==sizeof(fd),"child fd");
 char source[96];snprintf(source,sizeof(source),"/proc/%d/fd/%d",child,fd);
 errno=0;
 check(linkat(AT_FDCWD,source,repo,"objects/foreign",AT_SYMLINK_FOLLOW)==-1 && errno==EXDEV,"foreign tmpfile denied");
 check(write(ack[1],"x",1)==1,"ack write");
 int status;check(waitpid(child,&status,0)==child && status==0,"child status");
 close(unrelated);close(send[0]);close(send[1]);close(ack[0]);close(ack[1]);
}
int main(int argc,char **argv){
 check(argc==2,"args");
 serialized=getenv("TMPFILE_SERIALIZED")!=NULL;
 keep_errors=getenv("TMPFILE_KEEP_ERRORS")!=NULL;
 check(unshare(CLONE_NEWNS|CLONE_NEWUSER)==0,"unshare");
 check(mount(argv[1],"/tmpfile-test",NULL,MS_BIND,NULL)==0,"mount");
 directory("/tmpfile-test/installation");directory("/tmpfile-test/installation/repo");
 directory("/tmpfile-test/installation/repo/tmp");directory("/tmpfile-test/installation/repo/objects");
 /* Check mount identity before concurrent mutation. */
 int probe=open("/tmpfile-test/installation/repo/tmp",O_TMPFILE|O_RDWR,0600);
 check(probe>=0,"probe open");
 char via[96];snprintf(via,sizeof(via),"/proc/thread-self/fd/%d",probe);
 check(linkat(AT_FDCWD,via,AT_FDCWD,"/tmpfile-test/installation/repo/objects/probe",AT_SYMLINK_FOLLOW)==0,"thread-self probe linkat");
 close(probe);check(unlink("/tmpfile-test/installation/repo/objects/probe")==0,"probe unlink");
 repo=open("/tmpfile-test/installation/repo",O_RDONLY|O_DIRECTORY);check(repo>=0,"repo fd");
 pthread_t threads[N];
 check(pthread_barrier_init(&barrier,NULL,N)==0,"barrier");
 for(long i=0;i<N;i++)check(pthread_create(&threads[i],NULL,worker,(void *)i)==0,"pthread_create");
 for(int i=0;i<N;i++)check(pthread_join(threads[i],NULL)==0,"pthread_join");
 if(failures)return 1;
 foreign_process();close(repo);
 puts("PASS 8000 concurrent tmpfiles: same/cross thread, four proc-fd forms, nested repo bind, foreign rejection");
 return 0;
}
