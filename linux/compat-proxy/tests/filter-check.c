#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
int main(void){
 errno=0;if(syscall(SYS_getppid)!=-1 || errno!=EACCES)return 1;
 struct stat st;if(stat("/var/tmp",&st) || (st.st_mode&01777)!=01777)return 2;
 int anonymous=open("/var/tmp",O_TMPFILE|O_RDWR|O_CLOEXEC,0600);
 if(anonymous<0 && errno!=EOPNOTSUPP && errno!=ENOSYS && errno!=EISDIR)return 3;
 if(anonymous>=0){if(write(anonymous,"tmp",3)!=3)return 4;close(anonymous);}
 char path[96];snprintf(path,sizeof(path),"/var/tmp/matonos-tmp-%ld",(long)getpid());
 int named=open(path,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
 if(named<0 || write(named,"tmp",3)!=3)return 5;
 close(named);if(unlink(path))return 6;
 puts("filter-ok tmpdir-ok");return 0;
}
