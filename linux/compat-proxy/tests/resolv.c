#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <resolv.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s errno=%d\n",__LINE__,#x,errno);return 1;}}while(0)
int main(int argc,char **argv){
    CHECK(argc==2);
    if(!strcmp(argv[1],"isolated")){
        CHECK(access("/etc/resolv.conf",F_OK)==-1 && errno==ENOENT);
        CHECK(access("/etc/../etc/resolv.conf",F_OK)==-1 && errno==ENOENT);
        CHECK(access("/run/host/monitor/resolv.conf",F_OK)==-1 && errno==ENOENT);
        int fd=socket(AF_INET,SOCK_DGRAM,0);
        if(fd>=0){struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(53)};inet_pton(AF_INET,"127.10.42.99",&a.sin_addr);CHECK(connect(fd,(void *)&a,sizeof(a))==-1);close(fd);}
        struct addrinfo *answer=NULL,hints={.ai_family=AF_INET};
        CHECK(getaddrinfo("example.test",NULL,&hints,&answer)!=0);
        puts("PASS isolated resolver absent and getaddrinfo denied");
    }else{
        struct __res_state state={0};CHECK(!res_ninit(&state));
        CHECK(state.nscount==1);
        CHECK(state.nsaddr_list[0].sin_port==htons(53));
        CHECK(state.nsaddr_list[0].sin_addr.s_addr==inet_addr("127.10.42.99"));
        res_nclose(&state);puts("PASS glibc resolver loaded per-app 127.x:53 from Flatpak bind");
    }
    return 0;
}
