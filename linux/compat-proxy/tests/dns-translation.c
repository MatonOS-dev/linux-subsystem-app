/* Exercise production translation with a mocked kernel; no network permission. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../proxy.c"
static int expected_port=5353,calls;
static struct sockaddr_in kernel_peer;
void *vfs_alloc(size_t n){return malloc(n);}
void vfs_free(void *p){free(p);}
void p_copy(void *out,const void *in,size_t n){memcpy(out,in,n);}
static void destination(const void *pointer,unsigned long length) {
 assert(length==sizeof(struct sockaddr_in));
 const struct sockaddr_in *address=pointer;
 assert(address->sin_family==AF_INET && dns_swap(address->sin_port)==expected_port);
}
long proxy_raw(long nr,long a,long b,long c,long d,long e,long f) {
 (void)a;(void)f;calls++;
 if(nr==SYS_connect){destination((void *)b,c);return 0;}
 if(nr==SYS_sendto){if(e)destination((void *)e,f);return c;}
 if(nr==SYS_sendmsg){struct msghdr *m=(void *)b;if(m->msg_name)destination(m->msg_name,m->msg_namelen);return m->msg_iov[0].iov_len;}
 if(nr==SYS_sendmmsg){struct mmsghdr *m=(void *)b;for(long i=0;i<c;i++){if(m[i].msg_hdr.msg_name)destination(m[i].msg_hdr.msg_name,m[i].msg_hdr.msg_namelen);m[i].msg_len=7;}return c;}
 if(nr==SYS_getpeername){unsigned int *length=(void *)c;unsigned int size=*length<sizeof(kernel_peer)?*length:sizeof(kernel_peer);memcpy((void *)b,&kernel_peer,size);*length=sizeof(kernel_peer);return 0;}
 (void)d;assert(0);return -1;
}
int main(void) {
 assert(proxy_dns_forwarder("127.10.42.99:5353")==0);
 struct sockaddr_in address=dns_endpoint;address.sin_port=dns_swap(53);
 long args[6]={1,(long)&address,sizeof(address),0,0,0};
 assert(dns_call(SYS_connect,args)==0 && address.sin_port==dns_swap(53));
 /* glibc-style connected UDP sends carry no destination. */
 char query[7]={0};args[1]=(long)query;args[2]=sizeof(query);assert(dns_call(SYS_sendto,args)==7);
 /* musl-style unconnected sendto. */
 args[4]=(long)&address;args[5]=sizeof(address);assert(dns_call(SYS_sendto,args)==7 && address.sin_port==dns_swap(53));
 struct iovec iov={query,sizeof(query)};
 struct mmsghdr messages[2]={0};
 for(int i=0;i<2;i++)messages[i].msg_hdr=(struct msghdr){.msg_name=&address,.msg_namelen=sizeof(address),.msg_iov=&iov,.msg_iovlen=1};
 args[1]=(long)&messages[0].msg_hdr;args[2]=0;assert(dns_call(SYS_sendmsg,args)==7);
 args[1]=(long)messages;args[2]=2;assert(dns_call(SYS_sendmmsg,args)==2 && messages[0].msg_len==7 && messages[1].msg_len==7);
 assert(messages[0].msg_hdr.msg_name==&address && address.sin_port==dns_swap(53));
 dns_dup(1,2);assert(dns_tracked(2));dns_close(2);assert(!dns_tracked(2));
 kernel_peer=dns_endpoint;struct sockaddr_in peer={0};unsigned int length=sizeof(peer);
 args[1]=(long)&peer;args[2]=(long)&length;assert(dns_call(SYS_getpeername,args)==0 && peer.sin_port==dns_swap(53));
 length=3;memset(&peer,0,sizeof(peer));assert(dns_call(SYS_getpeername,args)==0 && length==sizeof(peer));
 assert(((unsigned char *)&peer)[3]==0);
 kernel_peer.sin_addr.s_addr++;length=sizeof(peer);assert(dns_call(SYS_getpeername,args)==0 && peer.sin_port==dns_swap(5353));
 address.sin_port=dns_swap(54);expected_port=54;args[1]=(long)&address;args[2]=sizeof(address);assert(dns_call(SYS_connect,args)==0);
 address.sin_port=dns_swap(53);address.sin_addr.s_addr++;expected_port=53;assert(dns_call(SYS_connect,args)==0);
 args[2]=2;struct sockaddr_in translated;assert(!dns_address(&address,2,&translated));
 const char *invalid[]={"127.10.42.99:53","1.2.3.4:5353","127.10.42.999:5353","127.10.42.99:65536","127.10.42.99:","127.10.42.99:5353junk"};
 for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);i++)assert(proxy_dns_forwarder(invalid[i])==-EINVAL);
 assert(calls>0);puts("PASS production DNS translation: connected UDP/TCP, sendto, messages, peer, native endpoints, invalid config");
}
