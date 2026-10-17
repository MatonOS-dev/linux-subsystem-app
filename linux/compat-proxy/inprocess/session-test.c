/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "session.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static long call(struct resident_session *s,long n,long a,long b,long c,long d,long e,long f){long v[]={a,b,c,d,e,f};return resident_session_call(s,n,v);}
static void *reply(void *arg){int server=*(int *)arg,client=accept(server,NULL,NULL);assert(client>=0);char data[8];assert(read(client,data,sizeof(data))==5);assert(!memcmp(data,"query",5));assert(write(client,"reply",5)==5);close(client);return NULL;}
int main(int argc,char **argv){
    assert(argc==3);int outputs[]={1,2};
    for(int run=0;run<2;run++){
        struct resident_session *s=resident_session_begin(outputs);assert(s);
        assert(!resident_session_bind(s,"/dev/null","/v-null"));
        long fd=call(s,SYS_openat,AT_FDCWD,(long)"/v-null",O_RDONLY,0,0,0);assert(fd==3);assert(!call(s,SYS_close,fd,0,0,0,0,0));
        assert(!resident_session_pack(s,"/packed",argv[1],argv[2]));
        fd=call(s,SYS_openat,AT_FDCWD,(long)"/packed/data",O_RDONLY,0,0,0);assert(fd==3);char bytes[32];assert(call(s,SYS_read,fd,(long)bytes,sizeof(bytes),0,0,0)==16);assert(!memcmp(bytes,"0123456789abcdef",16));
        long alias=call(s,SYS_dup,fd,0,0,0,0,0);assert(alias==4);assert(call(s,SYS_lseek,alias,0,SEEK_SET,0,0,0)==0);assert(call(s,SYS_read,fd,(long)bytes,1,0,0,0)==1 && bytes[0]=='0');
        char temporary[]="/tmp/resident-dns-XXXXXX";assert(mkdtemp(temporary));char path[100];snprintf(path,sizeof(path),"%s/dns",temporary);
        struct sockaddr_un address={.sun_family=AF_UNIX};snprintf(address.sun_path,sizeof(address.sun_path),"%s.udp",path);
        int server=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);assert(server>=0);assert(!bind(server,(void *)&address,sizeof(address)));assert(!listen(server,1));pthread_t responder;assert(!pthread_create(&responder,NULL,reply,&server));
        assert(!resident_session_dns(s,"127.10.42.99:53",path));
        long sock=call(s,SYS_socket,AF_INET,SOCK_DGRAM,0,0,0,0);assert(sock>=0);
        struct sockaddr_in endpoint={.sin_family=AF_INET,.sin_port=htons(53)};assert(inet_pton(AF_INET,"127.10.42.99",&endpoint.sin_addr)==1);
        assert(call(s,SYS_sendto,sock,(long)"query",5,0,(long)&endpoint,sizeof(endpoint))==5);
        assert(call(s,SYS_recvfrom,sock,(long)bytes,sizeof(bytes),0,0,0)==5 && !memcmp(bytes,"reply",5));
        assert(!pthread_join(responder,NULL));close(server);unlink(address.sun_path);rmdir(temporary);
        resident_session_end(s);
    }
    puts("resident VFS/pack/DNS session teardown PASS");return 0;
}
