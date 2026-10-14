#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
int main(void){
 int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return 1;
 struct sockaddr_un a;memset(&a,0xa5,sizeof(a));a.sun_family=AF_UNIX;
 strcpy(a.sun_path,"/tmp/maton-wsi-trace");
 (void)connect(fd,(struct sockaddr*)&a,sizeof(a));
 const unsigned char abstract[]={0,'w','s','i',0,0xff};
 memcpy(a.sun_path,abstract,sizeof(abstract));
 (void)connect(fd,(struct sockaddr*)&a,offsetof(struct sockaddr_un,sun_path)+sizeof(abstract));
 close(fd);return 0;
}
