/* SPDX-License-Identifier: Apache-2.0
 * Layer-owned logd client.
 *
 * Bionic's liblog caches exactly one descriptor, its /dev/socket/logdw dgram
 * socket, in the first process image that logs and keeps it for the life of
 * that image: LogdSocket::GetSocket() exchanges it into a static atomic and
 * LogdWrite() writes through the same number again and again
 * (system/logging/liblog/logd_writer.cpp). In a MatonOS payload process that
 * descriptor sits in the Linux payload's own descriptor table, but the layer
 * never learns its number: liblog creates it from bionic code inside an exempt
 * native range, so no proxy rule traps the socket()/connect(). A payload
 * descriptor sweep (Chromium's LaunchProcess closes every descriptor it does
 * not keep) then closes the logd socket and a later payload allocation
 * recycles the number; the next bionic log write lands in payload data, and
 * with SIGPIPE back at SIG_DFL in that child an EPIPE is fatal.
 *
 * The layer therefore installs its own liblog logger
 * (proxy_android_log_install) and speaks logd's wire protocol itself, so
 * liblog never owns a descriptor here at all. The layer's socket is a
 * private-reserve descriptor: payload close() of the number is refused,
 * close_range() sweeps step over it, and a payload dup2/dup3 onto the number
 * relocates the layer socket first (proxy_fd_number_moved). Writes go through
 * proxy_internal_send(), which carries MSG_NOSIGNAL, so a logd write can never
 * raise SIGPIPE in the payload.
 *
 * Wire format (system/logging/liblog/logd_writer.cpp, private/android_logger.h):
 * an 11-byte packed header {u8 id, u16 tid, u32 sec, u32 nsec} followed by
 * "<priority byte><tag NUL><message NUL>".
 */
#ifndef MATON_ANDROID_LOG_H
#define MATON_ANDROID_LOG_H
#include <time.h>
/* The Android hook and the host fixtures use these; a build that has neither
 * still compiles them for the shared code paths. */
#define ANDROID_LOG_MAYBE_UNUSED __attribute__((unused))
#define ANDROID_LOG_HEADER_BYTES 11u
/* LOGGER_ENTRY_MAX_PAYLOAD: logd drops larger datagrams. */
#define ANDROID_LOG_MAX_PAYLOAD 4068u
#define ANDROID_LOG_ID_DEFAULT 0x7fffffff
static int proxy_log_fd=-1;
static int proxy_log_opening;
static const char *proxy_log_path="/dev/socket/logdw";
static void android_log_le32(unsigned char *p,uint32_t v) {
    p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16);p[3]=(unsigned char)(v>>24);
}
static void android_log_relocated(int oldfd,int newfd){
    if(proxy_log_fd==oldfd)proxy_log_fd=newfd;
}
/* Host fixtures point the layer at their own listener. */
static ANDROID_LOG_MAYBE_UNUSED void proxy_android_log_path(const char *path){if(path&&*path)proxy_log_path=path;}
static ANDROID_LOG_MAYBE_UNUSED int proxy_android_log_descriptor(void){return proxy_log_fd;}
static long android_log_connect(int fd) {
    struct sockaddr_un address={.sun_family=AF_UNIX};
    size_t length=p_len(proxy_log_path);
    if(!length || length>=sizeof(address.sun_path))return -ENAMETOOLONG;
    p_copy(address.sun_path,proxy_log_path,length+1);
    return RAW(PROXY_SC_connect,fd,&address,offsetof(struct sockaddr_un,sun_path)+length+1,0,0,0);
}
/* Logd needs no credentials, so a datagram socket is enough. No SOCK_CLOEXEC:
 * a kernel exec into the peer loader must keep the number so the sealed state
 * stream can hand the same descriptor to the next resident and validate it. */
static int android_log_open(void) {
    if(!proxy_fd_reserve_ready)return -EMFILE;
    long fd=RAW(PROXY_SC_socket,AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0,0,0,0);
    if(fd<0)return (int)fd;
    long reserved=proxy_private_fd(fd);
    if(reserved<0){RAW(PROXY_SC_close,fd,0,0,0,0,0);return (int)reserved;}
    long connected=android_log_connect((int)reserved);
    if(connected<0){
        proxy_private_fd_release((int)reserved);
        RAW(PROXY_SC_close,reserved,0,0,0,0,0);
        return (int)connected;
    }
    return (int)reserved;
}
/* One opener at a time: proxy_private_fd() edits the shared reserve. A thread
 * that loses the race drops its line instead of allocating a second socket. */
static int android_log_fd(void) {
    int fd=__atomic_load_n(&proxy_log_fd,__ATOMIC_ACQUIRE);
    if(fd>=0)return fd;
    if(__atomic_exchange_n(&proxy_log_opening,1,__ATOMIC_ACQ_REL))return -EAGAIN;
    fd=android_log_open();
    __atomic_store_n(&proxy_log_fd,fd,__ATOMIC_RELEASE);
    __atomic_store_n(&proxy_log_opening,0,__ATOMIC_RELEASE);
    return fd;
}
/* Mirrors liblog's default logd logger, including the truncation limit, but
 * through the layer's private descriptor. */
static ANDROID_LOG_MAYBE_UNUSED long proxy_android_log_write(int buffer_id,int priority,const char *tag,const char *message) {
    unsigned char record[ANDROID_LOG_HEADER_BYTES+ANDROID_LOG_MAX_PAYLOAD];
    size_t capacity=sizeof(record),n=ANDROID_LOG_HEADER_BYTES;
    if(!tag)tag="";
    if(!message)message="";
    record[n++]=(unsigned char)priority;
    for(const char *s=tag;*s && n+1<capacity;n++)record[n]=*s++;
    record[n++]=0;
    for(const char *s=message;*s && n+1<capacity;n++)record[n]=*s++;
    record[n++]=0;
    record[0]=(unsigned char)(buffer_id==ANDROID_LOG_ID_DEFAULT?0:buffer_id);
    uint16_t tid=(uint16_t)RAW(PROXY_SC_gettid,0,0,0,0,0,0);
    record[1]=(unsigned char)tid;record[2]=(unsigned char)(tid>>8);
    struct timespec now={0,0};
    RAW(PROXY_SC_clock_gettime,CLOCK_REALTIME,&now,0,0,0,0);
    android_log_le32(record+3,(uint32_t)now.tv_sec);
    android_log_le32(record+7,(uint32_t)now.tv_nsec);
    int fd=android_log_fd();
    if(fd<0)return fd;
    long result=proxy_internal_send(fd,record,n,0);
    /* EAGAIN is logd backpressure; any other error means the peer went away,
     * so re-bind the peer on the same descriptor and retry once, as liblog
     * does. MSG_NOSIGNAL keeps the failure out of the payload's signal state. */
    if(result<0 && result!=-EAGAIN && android_log_connect(fd)>=0)
        result=proxy_internal_send(fd,record,n,0);
    return result;
}
static void android_log_state(struct state *s) {
    int fd=(int)state_word(s,(uint64_t)(int64_t)proxy_log_fd);
    if(!s->reading)return;
    proxy_log_fd=(fd>=0 && RAW(PROXY_SC_fcntl,fd,F_GETFD,0,0,0,0)>=0)?fd:-1;
}
#endif
