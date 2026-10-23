/* Host integration test for the async DNS helper, with a mocked libandroid
 * resolver and real host UDP/epoll/pthread primitives. */
#define _GNU_SOURCE
#define DNS_PROXY_PATH "/tmp/matonos-dnsproxyd-test"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "../proxy.c"

int proxy_diagnostic_fd=2;
void *vfs_alloc(size_t n){return malloc(n);}
void *vfs_palloc(size_t n){return malloc(n);}
void vfs_free(void *p){free(p);}
void p_copy(void *out,const void *in,size_t n){memcpy(out,in,n);}
size_t p_len(const char *s){return strlen(s);}
int p_equal(const char *a,const char *b){return strcmp(a,b)==0;}
char *vfs_resolve(int fd,const char *p,int follow,long *e){(void)fd;(void)follow;(void)e;return strdup(p);}

static pthread_mutex_t resolver_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned resolver_calls;
static long raw(long nr,long a,long b,long c,long d,long e,long f){return syscall(nr,a,b,c,d,e,f);}
long proxy_raw(long nr,long a,long b,long c,long d,long e,long f){return raw(nr,a,b,c,d,e,f);}
struct test_thread_start {void (*entry)(void *);void *arg;};
static void *test_thread_entry(void *p){struct test_thread_start *s=p;void (*entry)(void *)=s->entry;void *arg=s->arg;free(s);entry(arg);return NULL;}
long proxy_raw_clone_thread(unsigned long flags,void *stack,void (*entry)(void *),void *arg){
    (void)flags;(void)stack;pthread_t t;struct test_thread_start *s=malloc(sizeof(*s));if(!s)return -ENOMEM;*s=(struct test_thread_start){entry,arg};int r=pthread_create(&t,NULL,test_thread_entry,s);if(r)free(s);return r?-EAGAIN:0;
}
static void write_be32(int fd,uint32_t n){uint32_t b=htonl(n);assert(write(fd,&b,4)==4);}
static int b64v(char c){const char *p=strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",c);return p?(int)(p-"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"):-1;}
static void *mock_dnsproxyd(void *unused){
    (void)unused;int s=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(s>=0);
    struct sockaddr_un a={.sun_family=AF_UNIX};strcpy(a.sun_path,DNS_PROXY_PATH);unlink(a.sun_path);
    assert(bind(s,(struct sockaddr *)&a,sizeof(a))==0&&listen(s,32)==0);
    for(;;){int fd=accept(s,NULL,NULL);if(fd<0)continue;char cmd[4096];size_t n=0;while(n<sizeof(cmd)){ssize_t r=read(fd,cmd+n,sizeof(cmd)-n);if(r<=0)break;n+=(size_t)r;if(memchr(cmd,0,n))break;}
        static const char expected[]="resnsend 0 0 EjQBAAABAAAAAAAAAXgAAAEAAQ==\0";
        if(resolver_calls==0)assert(n==sizeof(expected)-1&&!memcmp(cmd,expected,n));
        assert(n>13&&!memcmp(cmd,"resnsend 0 0 ",13)&&cmd[n-1]==0);
        unsigned char answer[512];size_t z=0;int acc=0,bits=0;
        for(size_t i=13;i+1<n;i++){if(cmd[i]=='=')break;int v=b64v(cmd[i]);assert(v>=0);acc=(acc<<6)|v;bits+=6;if(bits>=8){bits-=8;answer[z++]=(unsigned char)(acc>>bits);}}
        assert(z>=12&&z<=sizeof(answer));answer[2]|=0x80;
        write_be32(fd,0);write_be32(fd,z);assert(write(fd,answer,z)==(ssize_t)z);
        pthread_mutex_lock(&resolver_lock);resolver_calls++;pthread_mutex_unlock(&resolver_lock);close(fd);
    }return NULL;
}
static int make_udp(void){proxy_arg_t a[6]={AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0};return (int)dns_call(PROXY_SC_socket,a);}
static void send_dns(int fd,int connected,const unsigned char *query,size_t size){
    struct sockaddr_in dns=dns_endpoint;proxy_arg_t a[6]={fd,0,size,0,0,0};
    if(connected){a[1]=(uintptr_t)query;assert(dns_call(PROXY_SC_sendto,a)==(long)size);}
    else {a[1]=(uintptr_t)query;a[4]=(uintptr_t)&dns;a[5]=sizeof(dns);assert(dns_call(PROXY_SC_sendto,a)==(long)size);}
}
static void send_dns_msg(int fd,const unsigned char *query,size_t size){
    struct sockaddr_in dns=dns_endpoint;struct iovec iov={(void *)query,size};
    struct msghdr msg={.msg_name=&dns,.msg_namelen=sizeof(dns),.msg_iov=&iov,.msg_iovlen=1};
    proxy_arg_t a[6]={fd,(uintptr_t)&msg,0};assert(dns_call(PROXY_SC_sendmsg,a)==(long)size);
}
static void send_dns_mmsg(int fd,const unsigned char *query,size_t size){
    struct sockaddr_in dns[2]={dns_endpoint,dns_endpoint};struct iovec iov[2]={{(void *)query,size},{(void *)query,size}};
    struct arch_mmsghdr msgs[2]={0};for(int i=0;i<2;i++){msgs[i].msg_hdr.msg_name=&dns[i];msgs[i].msg_hdr.msg_namelen=sizeof(dns[i]);msgs[i].msg_hdr.msg_iov=&iov[i];msgs[i].msg_hdr.msg_iovlen=1;}
    proxy_arg_t a[6]={fd,(uintptr_t)msgs,2,0};assert(dns_call(PROXY_SC_sendmmsg,a)==2);assert(msgs[0].msg_len==size&&msgs[1].msg_len==size);
}
static void receive_dns(int fd,const unsigned char *query,size_t size){
    unsigned char answer[512];struct sockaddr_in from={0};unsigned from_len=sizeof(from);
    struct pollfd p={.fd=fd,.events=POLLIN};assert(poll(&p,1,1500)==1&&(p.revents&POLLIN));
    proxy_arg_t a[6]={fd,(uintptr_t)answer,sizeof(answer),MSG_PEEK,(uintptr_t)&from,(uintptr_t)&from_len};
    assert(dns_call(PROXY_SC_recvfrom,a)==(long)size);
    assert(!memcmp(answer,query,2)&&answer[2]==(unsigned char)(query[2]|0x80));
    from=(struct sockaddr_in){0};from_len=sizeof(from);a[3]=0;
    assert(dns_call(PROXY_SC_recvfrom,a)==(long)size);
    assert(!memcmp(answer,query,2)&&answer[2]==(unsigned char)(query[2]|0x80));
    assert(!memcmp(answer+3,query+3,size-3));
    assert(from.sin_family==AF_INET&&from.sin_port==dns_endpoint.sin_port&&from.sin_addr.s_addr==dns_endpoint.sin_addr.s_addr);
}
static void receive_dns_epoll(int fd,const unsigned char *query,size_t size){
    int ep=epoll_create1(EPOLL_CLOEXEC);assert(ep>=0);struct epoll_event e={.events=EPOLLIN,.data.fd=fd};assert(epoll_ctl(ep,EPOLL_CTL_ADD,fd,&e)==0);
    struct epoll_event out={0};assert(epoll_wait(ep,&out,1,1500)==1&&(out.events&EPOLLIN));close(ep);
    unsigned char answer[512];struct sockaddr_in from={0};unsigned len=sizeof(from);proxy_arg_t a[6]={fd,(uintptr_t)answer,sizeof(answer),0,(uintptr_t)&from,(uintptr_t)&len};
    assert(dns_call(PROXY_SC_recvfrom,a)==(long)size&&answer[2]==(unsigned char)(query[2]|0x80));
}
static void receive_dns_blocking(int fd,const unsigned char *query,size_t size){
    struct timeval timeout={.tv_sec=2};assert(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);
    unsigned char answer[512];struct sockaddr_in from={0};unsigned len=sizeof(from);proxy_arg_t a[6]={fd,(uintptr_t)answer,sizeof(answer),0,(uintptr_t)&from,(uintptr_t)&len};
    assert(dns_call(PROXY_SC_recvfrom,a)==(long)size&&answer[2]==(unsigned char)(query[2]|0x80));
}
static void test_netd_result_errors(void){
    int p[2],rcode=0;unsigned char ans[8],err[]={0xff,0xff,0xff,0xf3,0,0,0,0};
    unsigned char nx[]={0,0,0,3,0,0,0,0};assert(socketpair(AF_UNIX,SOCK_STREAM,0,p)==0);assert(write(p[1],nx,sizeof(nx))==sizeof(nx));
    assert(dns_netd_result(p[0],&rcode,ans,sizeof(ans))==0&&rcode==3);close(p[0]);close(p[1]);
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,p)==0);assert(write(p[1],err,sizeof(err))==sizeof(err));
    assert(dns_netd_result(p[0],&rcode,ans,sizeof(ans))==-EACCES);close(p[0]);close(p[1]);
    unsigned char truncated[]={0,0,0,0,0,0,0,4,1,2};assert(socketpair(AF_UNIX,SOCK_STREAM,0,p)==0);
    assert(write(p[1],truncated,sizeof(truncated))==sizeof(truncated));close(p[1]);assert(dns_netd_result(p[0],&rcode,ans,sizeof(ans))==-EIO);close(p[0]);
}
static void *metadata_churn(void *unused){
    (void)unused;
    for(int i=0;i<1000;i++){int fd=make_udp();assert(fd>=0);dns_close(fd);}
    return NULL;
}
int main(void){
    test_netd_result_errors();
    assert(!proxy_dns_forwarder("127.0.0.53:53"));
    pthread_t server;assert(!pthread_create(&server,NULL,mock_dnsproxyd,NULL));usleep(20000);
    static const unsigned char query[]={0x12,0x34,1,0,0,1,0,0,0,0,0,0,1,'x',0,0,1,0,1};
    int connected=make_udp();assert(connected>=0);proxy_arg_t c[6]={connected,(uintptr_t)&dns_endpoint,sizeof(dns_endpoint)};
    assert(dns_call(PROXY_SC_connect,c)==0);send_dns(connected,1,query,sizeof(query));receive_dns(connected,query,sizeof(query));

    int to=make_udp();assert(to>=0);send_dns(to,0,query,sizeof(query));receive_dns(to,query,sizeof(query));

    /* Two outstanding query IDs stand in for libc's parallel A and AAAA
     * requests; the helper must route each resolver fd to its own socket. */
    unsigned char query_a[sizeof(query)],query_aaaa[sizeof(query)];
    memcpy(query_a,query,sizeof(query));memcpy(query_aaaa,query,sizeof(query));
    query_a[1]=0x40;query_aaaa[1]=0x41;query_aaaa[sizeof(query)-3]=28;
    int a_fd=make_udp(),aaaa_fd=make_udp();assert(a_fd>=0&&aaaa_fd>=0);
    send_dns(a_fd,0,query_a,sizeof(query_a));send_dns(aaaa_fd,0,query_aaaa,sizeof(query_aaaa));
    receive_dns(a_fd,query_a,sizeof(query_a));receive_dns(aaaa_fd,query_aaaa,sizeof(query_aaaa));

    int msg_fd=make_udp();assert(msg_fd>=0);send_dns_msg(msg_fd,query,sizeof(query));receive_dns_blocking(msg_fd,query,sizeof(query));
    int mmsg_fd=make_udp();assert(mmsg_fd>=0);send_dns_mmsg(mmsg_fd,query,sizeof(query));receive_dns_epoll(mmsg_fd,query,sizeof(query));receive_dns_epoll(mmsg_fd,query,sizeof(query));

    /* The helper must discard packets from ports not registered by this
     * process, without issuing an Android resolver query or waking this fd. */
    int ordinary=make_udp();assert(ordinary>=0);int foreign=(int)syscall(SYS_socket,AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(foreign>=0);
    assert(syscall(SYS_sendto,foreign,query,sizeof(query),0,&dns_helper_addr,sizeof(dns_helper_addr))==(long)sizeof(query));
    /* A subsequent owned request proves the helper drained the preceding
     * foreign datagram; only the owned packet may reach android_res_nsend. */
    send_dns(to,0,query,sizeof(query));receive_dns(to,query,sizeof(query));
    usleep(50000);pthread_mutex_lock(&resolver_lock);assert(resolver_calls==8);pthread_mutex_unlock(&resolver_lock);
    struct pollfd p={.fd=ordinary,.events=POLLIN};assert(poll(&p,1,50)==0);
    pthread_t churn[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&churn[i],NULL,metadata_churn,NULL));
    dns_fork_prepare();pid_t child=fork();assert(child>=0);
    if(!child){dns_fork_result(0);int child_fd=make_udp();if(child_fd<0)_exit(10);send_dns(child_fd,0,query,sizeof(query));receive_dns(child_fd,query,sizeof(query));_exit(0);}
    dns_fork_result(child);int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    usleep(30000);pthread_mutex_lock(&resolver_lock);assert(resolver_calls==9);pthread_mutex_unlock(&resolver_lock);
    for(unsigned i=0;i<4;i++)assert(!pthread_join(churn[i],NULL));
    close(foreign);dns_close(connected);dns_close(to);dns_close(ordinary);dns_close(a_fd);dns_close(aaaa_fd);dns_close(msg_fd);dns_close(mmsg_fd);
    puts("PASS async dnsproxyd helper: wire vector, errno/truncation, connected UDP, sendto/sendmsg/sendmmsg, parallel queries, poll/epoll, MSG_PEEK, foreign-source drop, fork restart, ordinary socket stays quiet");return 0;
}
