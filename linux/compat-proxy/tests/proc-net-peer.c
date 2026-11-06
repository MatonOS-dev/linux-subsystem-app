/* Keep a TCP fd alive in one proxy process; inspect it from another ABI. */
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc,char **argv,char **env){
    (void)env;
    if(argc==2){
        int fd=socket(AF_INET,SOCK_STREAM,0);
        struct sockaddr_in address={.sin_family=AF_INET,
            .sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=htons(atoi(argv[1]))};
        if(fd<0 || connect(fd,(void *)&address,sizeof(address)))return 2;
        char text[32];int n=snprintf(text,sizeof(text),"%d\n",fd);
        if(write(1,text,n)!=n)return 3;
        char done;if(read(0,&done,1)!=1)return 4;
        return close(fd)!=0;
    }
    if(argc!=3)return 5;
    int fd=open("/proc/net/tcp",O_RDONLY);
    char table[65536];ssize_t n=fd<0?-1:read(fd,table,sizeof(table)-1);
    if(n<0)return 6;
    table[n]=0;close(fd);
    char port[16],inode[48];
    snprintf(port,sizeof(port),":%04X ",atoi(argv[1]));
    snprintf(inode,sizeof(inode)," %s ",argv[2]);
    for(char *line=table;line && *line;){
        char *end=strchr(line,'\n');if(end)*end=0;
        if(strstr(line,port) && strstr(line,inode) && strstr(line," 01 "))return 0;
        line=end?end+1:NULL;
    }
    write(2,table,n);return 7;
}
