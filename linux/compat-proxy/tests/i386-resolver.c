/* SPDX-License-Identifier: Apache-2.0
 * Real compat glibc resolver, including UDP truncation -> TCP and timeout. */
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
int main(int argc,char **argv,char **env){
 (void)env;if(argc!=2)return 81;
 /* glibc orders addr before canonname; bionic orders canonname before addr. */
 struct glibc_addrinfo {int flags,family,socktype,protocol;unsigned len;struct sockaddr *addr;char *canon;struct glibc_addrinfo *next;};
 struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM},*answer=0;
 int r=getaddrinfo("v3-resolver.invalid",0,&hints,&answer);
 if(!strcmp(argv[1],"timeout")){if(r!=-3 && r!=-4)return 82;}
 else {if(r || !answer)return 83;
  struct sockaddr_in *a=(void *)((struct glibc_addrinfo *)answer)->addr;
  if(a->sin_addr.s_addr!=0x010200c0)return 84;freeaddrinfo(answer);
 }
 static const char ok[]="PASS i386 glibc resolver\n";return write(1,ok,sizeof(ok)-1)==sizeof(ok)-1?0:85;
}
