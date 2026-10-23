/* DNS proxy core smoke test with a mocked kernel. The libandroid helper loop
 * is covered by Android/QEMU integration; this verifies routing decisions. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../proxy.c"
int proxy_diagnostic_fd=2;
void *vfs_alloc(size_t n){return malloc(n);} void *vfs_palloc(size_t n){return malloc(n);} void vfs_free(void *p){free(p);}
void p_copy(void *out,const void *in,size_t n){memcpy(out,in,n);} size_t p_len(const char *s){return strlen(s);} int p_equal(const char *a,const char *b){return strcmp(a,b)==0;}
char *vfs_resolve(int fd,const char *p,int follow,long *e){(void)fd;(void)follow;(void)e;return strdup(p);}
static int nextfd=20,raw_names;
long proxy_raw(long nr,long a,long b,long c,long d,long e,long f){(void)a;(void)d;(void)f;(void)e;
 if(nr==SYS_rt_sigprocmask){if(c)*(unsigned long *)c=0;return 0;}
 if(nr==SYS_getpid)return 123;
 if(nr==SYS_socket)return nextfd++;
 if(nr==SYS_getsockname){struct sockaddr_in *out=(void *)b;*out=(struct sockaddr_in){.sin_family=AF_INET,.sin_port=dns_swap(32123)};*(unsigned *)c=sizeof(*out);raw_names++;return 0;}
 if(nr==SYS_connect)return 0;
 if(nr==SYS_close)return 0;
 assert(!"unexpected mocked syscall");return -1;
}
static int make_socket(int type){proxy_arg_t a[6]={AF_INET,type,0};return (int)dns_call(PROXY_SC_socket,a);}
int main(void){assert(!proxy_dns_forwarder("127.0.0.53:53"));struct sockaddr_in dns=dns_endpoint;int tcp=make_socket(SOCK_STREAM);proxy_arg_t a[6]={tcp,(uintptr_t)&dns,sizeof(dns)};assert(dns_call(PROXY_SC_connect,a)==-ECONNREFUSED);assert(dns_helper_state==0);
 int udp=make_socket(SOCK_DGRAM);struct sockaddr_in local={0};unsigned len=sizeof(local);a[0]=udp;a[1]=(uintptr_t)&local;a[2]=(uintptr_t)&len;assert(!dns_call(PROXY_SC_getsockname,a));assert(raw_names==1&&local.sin_port==dns_swap(32123));
 struct sockaddr_in foreign={.sin_family=AF_INET,.sin_port=dns_swap(53),.sin_addr={.s_addr=0x3500007f}};assert(dns_address(&foreign,sizeof(foreign)));foreign.sin_port=dns_swap(54);assert(!dns_address(&foreign,sizeof(foreign)));
 puts("PASS DNS endpoint parsing, TCP refusal, ordinary UDP names, and endpoint matching");return 0;}
