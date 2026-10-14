/* Original i386 ipc(117) multiplexer fixture, sharing the generic IPC engine. */
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/sem.h>
extern long syscall(long,...);
extern int *__errno_location(void);
struct operation {unsigned short num;short op;short flags;};
int main(int argc,char **argv,char **env){
 (void)argc;(void)argv;(void)env;
 long id=syscall(117,2,0,1,0600,0,0);if(id<0)return 31;
 uint32_t value=1;if(syscall(117,3,id,0,16,&value,0))return 32;
 struct operation op={0,-1,0};if(syscall(117,1,id,1,0,&op,0))return 33;
 struct {int64_t sec,nsec;} timeout={0,20000000};
 if(syscall(420,id,&op,1,&timeout)!=-1 || *__errno_location()!=11)return 39;
 value=0;if(syscall(117,3,id,0,0,&value,0))return 34;
 long shm=syscall(117,23,0,4096,0600,0,0);if(shm<0)return 35;
 uint32_t address=0;if(syscall(117,21,shm,0,&address,0,0))return 36;
 *(volatile int *)(uintptr_t)address=42;
 if(syscall(117,22,0,0,0,address,0))return 37;
 if(syscall(117,24,shm,0,0,0,0))return 38;
 long direct=syscall(393,0,1,0600);if(direct<0)return 60;
 struct semid64_ds stat;if(syscall(394,direct,0,0x102,&stat)<0 || stat.sem_nsems!=1){syscall(394,direct,0,0,0);return 61;}
 if(syscall(394,direct,0,0,0))return 62;
 long queue=syscall(117,13,0,0600,0,0,0);if(queue<0)return 63;
 struct {int32_t type;char text[8];} message={7,"message"},received={0};
 if(syscall(117,11,queue,8,0,&message,0))return 64;
 if(syscall(117,(1<<16)|12,queue,4,04000,&received,7)!=-1 || *__errno_location()!=7)return 65;
 struct {uint32_t pointer;int32_t type;} old={(uintptr_t)&received,7};
 if(syscall(117,12,queue,8,04000,&old,0)!=8 || received.type!=7 || received.text[0]!='m')return 66;
 if(syscall(117,(1<<16)|12,queue,8,04000,&received,7)!=-1 || *__errno_location()!=42)return 67;
 if(syscall(117,14,queue,0,0,0,0))return 68;
 static const char ok[]="PASS i386 ipc(117) semget/ctl/op and shmat output-pointer ABI\n";
 write(1,ok,sizeof(ok)-1);return 0;
}
