/* Nested X11/Wayland relays must preserve dma-buf/SHM/fence descriptors. */
#define _GNU_SOURCE
#include "../socket-relay.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int main(void) {
    int app[2],display[2];
    assert(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,app)==0);
    assert(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,display)==0);
    WaylandRelay* relay=malloc(sizeof(*relay));assert(relay);
    relay->client=app[1];relay->compositor=display[0];
    pthread_t thread;assert(pthread_create(&thread,NULL,relay_wayland,relay)==0);
    int fd=memfd_create("session-buffer",MFD_CLOEXEC);assert(fd>=0);
    assert(write(fd,"pixels",6)==6);
    char text[]="descriptor-bearing-frame";
    struct iovec payload={.iov_base=text,.iov_len=sizeof(text)};
    union {struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int))];} control={0};
    struct msghdr message={.msg_iov=&payload,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    struct cmsghdr* rights=CMSG_FIRSTHDR(&message);rights->cmsg_level=SOL_SOCKET;rights->cmsg_type=SCM_RIGHTS;rights->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(rights),&fd,sizeof(fd));
    assert(sendmsg(app[0],&message,MSG_NOSIGNAL)==sizeof(text));close(fd);
    char received[128]={0};payload.iov_base=received;payload.iov_len=sizeof(received);
    memset(control.bytes,0,sizeof(control.bytes));message.msg_controllen=sizeof(control.bytes);
    assert(recvmsg(display[1],&message,MSG_CMSG_CLOEXEC)==sizeof(text));assert(!strcmp(received,text));
    rights=CMSG_FIRSTHDR(&message);assert(rights && rights->cmsg_type==SCM_RIGHTS);
    memcpy(&fd,CMSG_DATA(rights),sizeof(fd));assert(fcntl(fd,F_GETFD)&FD_CLOEXEC);
    assert(pread(fd,received,6,0)==6 && !memcmp(received,"pixels",6));close(fd);
    assert(send(display[1],"reply",5,MSG_NOSIGNAL)==5);
    assert(recv(app[0],received,sizeof(received),0)==5 && !memcmp(received,"reply",5));
    shutdown(app[0],SHUT_RDWR);close(app[0]);assert(pthread_join(thread,NULL)==0);close(display[1]);
    puts("PASS: session display relay preserves buffer FDs, duplex bytes and disconnect cleanup");
    return 0;
}
