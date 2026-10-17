#define _GNU_SOURCE
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../linux/subsystem/native/short_socket_path.h"

int main(void) {
  char root[] = "/tmp/maton-short-socket-XXXXXX";
  assert(mkdtemp(root));
  char directory[512]; snprintf(directory, sizeof(directory), "%s", root);
  for (int i=0; i<5; i++) {
    size_t n=strlen(directory); snprintf(directory+n,sizeof(directory)-n,"/%040d",i);
    assert(!mkdir(directory,0700));
  }
  assert(strlen(directory)>200);
  char full[600]; snprintf(full,sizeof(full),"%s/endpoint",directory);
  int dirfd=-1; char path[108];
  int path_length=maton_socket_path_open(path,sizeof(path),full,&dirfd);
  assert(dirfd>=0 && path_length>0 && path_length==(int)strlen(path));
  int server=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0); assert(server>=0);
  struct sockaddr_un address={.sun_family=AF_UNIX}; strcpy(address.sun_path,path);
  assert(!bind(server,(struct sockaddr *)&address,offsetof(struct sockaddr_un,sun_path)+strlen(path)+1));
  assert(!listen(server,1));
  int client=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0); assert(client>=0);
  assert(!connect(client,(struct sockaddr *)&address,offsetof(struct sockaddr_un,sun_path)+strlen(path)+1));
  int accepted=accept(server,NULL,NULL); assert(accepted>=0);
  close(accepted); close(client); close(server); unlink(path); close(dirfd);
  char cmd[600]; snprintf(cmd,sizeof(cmd),"rm -rf '%s'",root); assert(system(cmd)==0);
  puts("short socket path host test passed");
  return 0;
}
