/* Production DNS transport against a mocked kernel: runnable without bind. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../proxy.c"
int proxy_diagnostic_fd=2;
void *vfs_alloc(size_t n){return malloc(n);}
void vfs_free(void *p){free(p);}
void p_copy(void *out,const void *in,size_t n){memcpy(out,in,n);}
size_t p_len(const char *s){return strlen(s);}
int p_equal(const char *x,const char *y){return strcmp(x,y)==0;}
char *vfs_resolve(int fd,const char *path,int follow,long *e){(void)fd;(void)follow;(void)e;return strdup(path);}
static int next_fd=10,unix_connects,duplicates,watches,options,bad_peer,kind;
static struct epoll_event event;
static int polls,clock_ticks;
long proxy_raw(long nr,long a,long b,long c,long d,long e,long f){
 (void)f;
 if(nr==SYS_pipe2){((int *)a)[0]=90;((int *)a)[1]=91;return 0;}
 if(nr==SYS_clock_gettime){struct timespec *t=(void *)b;t->tv_sec=0;t->tv_nsec=++clock_ticks*1000000;return 0;}
 if(nr==SYS_ppoll){struct timespec *t=(void *)c;assert(t->tv_sec==0 && t->tv_nsec<=10000000);return ++polls==2?1:0;}
 if(nr==SYS_rt_sigprocmask){if(c)*(unsigned long *)c=0;return 0;}
 if(nr==SYS_socket){if(a==AF_UNIX){kind=b&~(SOCK_CLOEXEC|SOCK_NONBLOCK);assert(kind==SOCK_SEQPACKET || kind==SOCK_STREAM);}return next_fd++;}
 if(nr==SYS_connect){const struct sockaddr_un *u=(void *)b;if(u->sun_family==AF_UNIX){assert(c==(long)(offsetof(struct sockaddr_un,sun_path)+strlen(u->sun_path)+1));assert(!strcmp(u->sun_path,kind==SOCK_STREAM?"/tmp/dns.tcp":"/tmp/dns.udp"));unix_connects++;}return 0;}
 if(nr==SYS_getuid)return 1234;
 if(nr==SYS_getsockopt){if(c==SO_PEERCRED){struct ucred *u=(void *)d;u->uid=bad_peer?999:1234;*(unsigned *)e=sizeof(*u);}else {memset((void *)d,0,8);*(unsigned *)e=8;}return 0;}
 if(nr==SYS_setsockopt){options++;return 0;}
 if(nr==SYS_getsockname){struct sockaddr_in in={.sin_family=AF_INET,.sin_port=dns_swap(12345)};memcpy((void *)b,&in,sizeof(in));*(unsigned *)c=sizeof(in);return 0;}
 if(nr==SYS_fcntl)return b==F_GETFL?O_NONBLOCK:b==F_GETFD?FD_CLOEXEC:0;
 if(nr==SYS_dup3){duplicates++;assert(c==O_CLOEXEC);return b;}
 if(nr==SYS_close)return 0;
 if(nr==SYS_epoll_ctl){if(b==EPOLL_CTL_ADD){assert(!memcmp((void *)d,&event,sizeof(event)));watches++;}return 0;}
 if(nr==SYS_sendto){assert(!e);return c;}
 if(nr==SYS_sendmsg){struct msghdr *m=(void *)b;assert(!m->msg_name && !m->msg_namelen);return m->msg_iov[0].iov_len;}
 if(nr==SYS_sendmmsg){struct mmsghdr *m=(void *)b;for(long i=0;i<c;i++){assert(!m[i].msg_hdr.msg_name);m[i].msg_len=7;}return c;}
 if(nr==SYS_recvfrom){assert(!e);memcpy((void *)b,"answer!",c<7?c:7);return c<7?c:7;}
 if(nr==SYS_recvmsg){struct msghdr *m=(void *)b;assert(!m->msg_name);m->msg_flags=MSG_EOR|MSG_TRUNC;m->msg_controllen=0;return 7;}
 assert(0);return -1;
}
static int create(int type){long a[6]={AF_INET,type,0};return (int)dns_call(PROXY_SC_socket,a);}
int main(void){
 assert(!proxy_dns_forwarder("127.10.42.99:53"));assert(!proxy_dns_socket("/tmp/dns"));
 int fd=create(SOCK_DGRAM);
 struct pollfd wait={.fd=fd,.events=POLLIN};struct timespec duration={.tv_nsec=50000000};
 assert(dns_poll(&wait,1,&duration,NULL,8)==1 && polls==2);
 dns_dup(fd,50);assert(dns_lookup(fd)==dns_lookup(50));
 event=(struct epoll_event){.events=EPOLLIN|EPOLLET,.data.u64=0xabcdef};
 long args[6]={100,EPOLL_CTL_ADD,fd,(long)&event};assert(!dns_call(PROXY_SC_epoll_ctl,args));
 struct sockaddr_in address=dns_endpoint;
 args[0]=fd;args[1]=(long)&address;args[2]=sizeof(address);args[3]=0;
 assert(!dns_call(PROXY_SC_connect,args));assert(unix_connects==1 && duplicates==2 && watches==2 && options==5);
 assert(dns_lookup(fd)->active && dns_lookup(fd)->connected);
 struct sockaddr_in peer={0};unsigned length=sizeof(peer);
 args[0]=50;args[1]=(long)&peer;args[2]=(long)&length;assert(!dns_call(PROXY_SC_getpeername,args));assert(!memcmp(&peer,&address,sizeof(peer)));
 length=3;memset(&peer,0,sizeof(peer));assert(!dns_call(PROXY_SC_getpeername,args) && length==sizeof(peer));assert(((char *)&peer)[3]==0);
 char query[7]={0};args[1]=(long)query;args[2]=7;args[3]=0;args[4]=(long)&address;args[5]=sizeof(address);
 assert(dns_call(PROXY_SC_sendto,args)==7);
 struct iovec iov={query,sizeof(query)};struct mmsghdr messages[2]={0};
 for(int i=0;i<2;i++)messages[i].msg_hdr=(struct msghdr){.msg_name=&address,.msg_namelen=sizeof(address),.msg_iov=&iov,.msg_iovlen=1};
 args[1]=(long)&messages[0].msg_hdr;args[2]=0;assert(dns_call(PROXY_SC_sendmsg,args)==7);
 args[1]=(long)messages;args[2]=2;assert(dns_call(PROXY_SC_sendmmsg,args)==2 && messages[1].msg_len==7);assert(messages[0].msg_hdr.msg_name==&address);
 args[1]=(long)query;args[2]=7;length=sizeof(peer);args[4]=(long)&peer;args[5]=(long)&length;assert(dns_call(PROXY_SC_recvfrom,args)==7);assert(!memcmp(&peer,&address,sizeof(peer)));
 messages[0].msg_hdr.msg_name=&peer;messages[0].msg_hdr.msg_namelen=sizeof(peer);args[1]=(long)&messages[0].msg_hdr;args[2]=0;assert(dns_call(PROXY_SC_recvmsg,args)==7);assert(messages[0].msg_hdr.msg_flags==MSG_TRUNC && !memcmp(&peer,&address,sizeof(peer)));
 int stream=create(SOCK_STREAM);args[0]=stream;args[1]=(long)&address;args[2]=sizeof(address);assert(!dns_call(PROXY_SC_connect,args) && kind==SOCK_STREAM);
 int rejected=create(SOCK_DGRAM);args[0]=rejected;bad_peer=1;assert(dns_call(PROXY_SC_connect,args)==-EACCES && !dns_lookup(rejected)->active);bad_peer=0;
 namespaces=CLONE_NEWNET;assert(dns_call(PROXY_SC_connect,args)==-EACCES);
 struct sockaddr_un unix_address={.sun_family=AF_UNIX};strcpy(unix_address.sun_path,"/tmp/dns.udp");
 args[1]=(long)&unix_address;args[2]=offsetof(struct sockaddr_un,sun_path)+strlen(unix_address.sun_path)+1;
 assert(dns_call(PROXY_SC_connect,args)==-EACCES);
 namespaces=0;
 dns_close(fd);assert(dns_lookup(50)->active);dns_close(50);assert(!dns_lookup(50));namespaces=CLONE_NEWNET;dns_network_payload();assert(!dns_lookup(stream));namespaces=0;dns_close(stream);dns_close(rejected);dns_close(100);
 const char *invalid[]={"1.2.3.4:53","127.1.2.3:5353","127.1.2.999:53","127.1.2.3:","127.1.2.3:53junk"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++)assert(proxy_dns_forwarder(invalid[i])==-EINVAL);
 puts("PASS Unix DNS production conversion: UDP/TCP, aliases, options, epoll replay, peer/source names, messages, wrong UID and no-network denial");
}
