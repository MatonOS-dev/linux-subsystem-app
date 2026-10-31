/* /proc/net/tcp lists this stub's connected and accepted loopback sockets by
 * inode (lsof needs it; Android denies the kernel table) and drops closed ones. */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
static char table[65536];
static void load(void){int fd=open("/proc/net/tcp",O_RDONLY);ssize_t n=fd<0?-1:read(fd,table,sizeof(table)-1);table[n<0?0:n]=0;if(fd>=0)close(fd);}
static int listed(int fd,unsigned port){struct stat st;fstat(fd,&st);
 char ino[32];snprintf(ino,sizeof(ino)," %lu ",(unsigned long)st.st_ino);char p[16];snprintf(p,sizeof(p),":%04X ",port);
 for(char *line=table;line && *line;){char *end=strchr(line,'\n');if(end)*end=0;int hit=strstr(line,ino) && strstr(line,p) && strstr(line," 01 ");if(end)*end='\n';if(hit)return 1;line=end?end+1:NULL;}return 0;}
int main(void){
 int l=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};socklen_t len=sizeof(a);
 if(bind(l,(void *)&a,sizeof(a))||listen(l,1)||getsockname(l,(void *)&a,&len))return 2;
 unsigned port=ntohs(a.sin_port);
 int c=socket(AF_INET,SOCK_STREAM,0);if(connect(c,(void *)&a,sizeof(a)))return 3;int s=accept(l,NULL,NULL);if(s<0)return 4;
 load();if(strncmp(table,"  sl  local_address",19))return 5;
 if(!listed(c,port)||!listed(s,port))return fprintf(stderr,"%s",table),6;
 struct stat st;fstat(c,&st);close(c);load();char ino[32];snprintf(ino,sizeof(ino)," %lu ",(unsigned long)st.st_ino);if(strstr(table,ino))return 7;
 if(!listed(s,port))return 8;
 puts("PASS /proc/net/tcp lists connected and accepted sockets");return 0;}
