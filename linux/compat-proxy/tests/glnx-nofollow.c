#define _GNU_SOURCE
#include <glnx-chase.h>
#include <glnx-fdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){
 if(argc!=2 || chdir(argv[1]))return 1;
 int fd=open("metadata",O_WRONLY|O_CREAT|O_EXCL,0600);
 if(fd<0 || write(fd,"metadata",8)!=8 || close(fd) || link("metadata","other"))return 2;
 GError *error=NULL;
 fd=glnx_chaseat(AT_FDCWD,"metadata",GLNX_CHASE_RESOLVE_NO_SYMLINKS|GLNX_CHASE_MUST_BE_REGULAR,&error);
 if(fd<0){fprintf(stderr,"glnx_chaseat: %s\n",error?error->message:"no error");return 3;}
 int readable=glnx_fd_reopen(fd,O_RDONLY|O_CLOEXEC,&error);close(fd);fd=readable;if(fd<0)return 5;
 char buf[8];if(read(fd,buf,8)!=8 || memcmp(buf,"metadata",8))return 4;
 close(fd);unlink("metadata");unlink("other");
 puts("PASS upstream glnx_chaseat NO_SYMLINKS|MUST_BE_REGULAR and fd reopen");return 0;
}
