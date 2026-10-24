/* SPDX-License-Identifier: Apache-2.0
 * Same original fixture built for both ABIs; each process changes class twice.
 * No separate loader invocation: payload exec goes through resident handoff. */
#define _GNU_SOURCE
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
extern int *__errno_location(void);
#undef errno
#define errno (*__errno_location())
#include <linux/sem.h>
extern long syscall(long,...);
static long semget_(int key){return sizeof(long)==4?syscall(117,2,key,1,0600,0,0):syscall(64,key,1,0600);}
static long semctl_(int id,int cmd,int value){uint32_t v=value;return sizeof(long)==4?syscall(117,3,id,0,cmd,&v,0):syscall(66,id,0,cmd,value);}
static long semop_(int id,int value,int flags){struct sembuf op={0,value,flags};return sizeof(long)==4?syscall(117,1,id,1,0,&op,0):syscall(65,id,&op,1);}
static long shmget_(void){return sizeof(long)==4?syscall(117,23,0,4096,0600,0,0):syscall(29,0,4096,0600);}
static void *shmat_(int id){if(sizeof(long)==8)return (void *)syscall(30,id,0,0);uint32_t address=0;if(syscall(117,21,id,0,&address,0,0))return (void *)-1;return (void *)(uintptr_t)address;}
static long shmctl_(int id){return sizeof(long)==4?syscall(117,24,id,0,0,0,0):syscall(31,id,0,0);}
static long msgget_(void){return sizeof(long)==4?syscall(117,13,0,0600,0,0,0):syscall(68,0,0600);}
static long msgctl_(int id){return sizeof(long)==4?syscall(117,14,id,0,0,0,0):syscall(71,id,0,0);}
struct message {long type;char text[16];};
static long send_(int id,struct message *m){return sizeof(long)==4?syscall(117,11,id,16,0,m,0):syscall(69,id,m,16,0);}
static long receive_(int id,struct message *m){return sizeof(long)==4?syscall(117,(1<<16)|12,id,16,04000,m,7):syscall(70,id,m,16,7,04000);}
static void number(char *out,int n){unsigned k=0;char rev[24];do{rev[k++]=n%10+'0';n/=10;}while(n);unsigned i=0;while(k)out[i++]=rev[--k];out[i]=0;}
static int fail(int line){char b[24];number(b,line);if(write(2,b,strlen(b))<0)return 1;if(write(2,"\n",1)<0)return 1;return 1;}
#define CHECK(x) do{if(!(x))return fail(__LINE__);}while(0)
int main(int argc,char **argv,char **env){
 CHECK(argc>=3);
 if(argc==3){
  int sem=semget_(0),shm=shmget_(),msg=msgget_();CHECK(sem>=0&&shm>=0&&msg>=0);
  CHECK(!semctl_(sem,16,3));int *memory=shmat_(shm);CHECK(memory!=(void *)-1);*memory=19;
  int fd=open("/v3-cross-file",O_RDONLY),alias=dup(fd),dead=open("/v3-cross-file",O_RDONLY|O_CLOEXEC);CHECK(fd>=0&&alias>=0&&dead>=0);
  struct message m={7,"from parent"};CHECK(!send_(msg,&m));
  int tmp=open("/v3-cross-dir/named",O_CREAT|O_TRUNC|O_RDWR|O_CLOEXEC,0600);CHECK(tmp>=0);
  int tmpalias=dup(tmp);CHECK(tmpalias>=0&&write(tmpalias,"inode",5)==5);
  char words[7][24];int values[]={sem,shm,msg,fd,alias,dead,tmpalias};for(unsigned i=0;i<7;i++)number(words[i],values[i]);
  pid_t child=fork();CHECK(child>=0);
  if(!child){char *args[]={argv[1],argv[2],argv[1],"one",words[0],words[1],words[2],words[3],words[4],words[5],words[6],NULL};execve(args[0],args,env);_exit(101);}
  int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  CHECK(*memory==42);CHECK(semctl_(sem,12,0)==3);CHECK(receive_(msg,&m)==16&&!strcmp(m.text,"from child"));
  CHECK(!semctl_(sem,0,0)&&!shmctl_(shm)&&!msgctl_(msg));close(fd);close(alias);close(dead);close(tmp);close(tmpalias);
  static const char ok[]="PASS mixed-class exec, VFS/fds and sem/shm/msg/SEM_UNDO\n";CHECK(write(1,ok,sizeof(ok)-1)==sizeof(ok)-1);return 0;
 }
 CHECK(argc==11);
 CHECK(ioctl(-1,0xc0306201UL,0)==-1&&errno==EBADF);
 int sem=atoi(argv[4]),shm=atoi(argv[5]),msg=atoi(argv[6]),fd=atoi(argv[7]),alias=atoi(argv[8]),dead=atoi(argv[9]),tmpalias=atoi(argv[10]);
 char inode[5];CHECK(pread(tmpalias,inode,5,0)==5&&!memcmp(inode,"inode",5));
 CHECK(fcntl(dead,F_GETFD)==-1&&errno==EBADF);
 char text[32]={0};CHECK(read(fd,text,5)==5&&!memcmp(text,"cross",5));CHECK(lseek(alias,0,SEEK_CUR)==5);CHECK(lseek(fd,0,SEEK_SET)==0);
 /* The diagnostic sink is duplicated at or above 512 (PROXY_PRIVATE_FD_FLOOR). Payload probes and
  * close cannot steal it; the second exec must still emit its trace. */
 int private_flags=fcntl(512,F_GETFD);
 /* Native F_GETFD may expose the private descriptor's kernel state. */
 CHECK(private_flags>=0 || errno==EBADF);
 if(private_flags>=0)CHECK(!fcntl(512,F_SETFD,FD_CLOEXEC));
 CHECK(close(512)==0);
 CHECK(read(512,text,1)==-1&&errno==EBADF);
 struct stat st;CHECK(!syscall(sizeof(long)==4?195:4,"/v3-cross-file",&st)&&st.st_size==6);
 int *memory=shmat_(shm);CHECK(memory!=(void *)-1);
 if(!strcmp(argv[3],"one")){
  CHECK(*memory==19);*memory=23;
  int removed=shmget_();CHECK(removed>=0&&shmat_(removed)!=(void *)-1);
  memory[1]=removed;CHECK(!shmctl_(removed));
  CHECK(!semop_(sem,-1,SEM_UNDO));
  struct message m;CHECK(receive_(msg,&m)==16&&m.type==7&&!strcmp(m.text,"from parent"));
  strcpy(m.text,"from child");CHECK(!send_(msg,&m));
  char *args[]={argv[1],argv[2],argv[1],"two",argv[4],argv[5],argv[6],argv[7],argv[8],argv[9],argv[10],NULL};execve(args[0],args,env);return 102;
 }
 CHECK(*memory==23);CHECK(shmat_(memory[1])==(void *)-1&&errno==EINVAL);*memory=42;CHECK(semctl_(sem,12,0)==2);
 return 0;
}
