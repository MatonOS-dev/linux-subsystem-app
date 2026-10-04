/* Exercise the private directory-FD registration and listener lifetime. */
#include "broker.c"
#include <assert.h>

static int registration(Broker* broker,int listener,const char* path,int* fds,unsigned count) {
    int client=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);assert(client>=0);
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,path);
    assert(connect(client,(struct sockaddr*)&address,sizeof(address))==0);
    struct MatonSessionRegistration registration={.pid=getpid()};
    strcpy(registration.monitor,"/data/matonos/linux/runtime/wayland-test-monitor");
    strcpy(registration.flatpak_version,"1.14.10");
    union {struct cmsghdr align;char bytes[CMSG_SPACE(2*sizeof(int))];} control={0};
    struct iovec payload={.iov_base=&registration,.iov_len=sizeof(registration)};
    struct msghdr packet={.msg_iov=&payload,.msg_iovlen=1};
    if(count) {
        packet.msg_control=control.bytes;packet.msg_controllen=CMSG_SPACE(count*sizeof(int));
        struct cmsghdr* rights=CMSG_FIRSTHDR(&packet);
        rights->cmsg_level=SOL_SOCKET;rights->cmsg_type=SCM_RIGHTS;
        rights->cmsg_len=CMSG_LEN(count*sizeof(int));memcpy(CMSG_DATA(rights),fds,count*sizeof(int));
    }
    assert(sendmsg(client,&packet,MSG_NOSIGNAL)==sizeof(registration));
    assert(control_accept(listener,G_IO_IN,broker)==G_SOURCE_CONTINUE);
    return client;
}
int main(void) {
    if(getuid()!=1000){puts("SKIP: requires native system UID 1000");return 77;}
    char directory[]="/tmp/maton-native-bus-XXXXXX";assert(mkdtemp(directory));
    char path[108],policy[128],bus[128];
    snprintf(path,sizeof(path),"%s/control",directory);
    snprintf(policy,sizeof(policy),"%s/policy",directory);
    snprintf(bus,sizeof(bus),"%s/bus",directory);
    assert(g_file_set_contents(policy,"own org.example.Test\n",-1,NULL));
    Broker* broker=broker_new(bus,policy,NULL);assert(broker);broker->host_session=TRUE;
    int listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);assert(listener>=0);
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,path);
    assert(bind(listener,(struct sockaddr*)&address,sizeof(address))==0);assert(listen(listener,8)==0);
    int native_fd=open(directory,O_DIRECTORY|O_CLOEXEC);assert(native_fd>=0);
    int file=open(policy,O_RDONLY|O_CLOEXEC);assert(file>=0);
    int duplicates[]={native_fd,native_fd};
    for(unsigned test=0;test<4;test++) {
        int* fds=test==1 ? &file : duplicates;
        unsigned count=test==0 ? 0 : test==3 ? 2 : 1;
        int client=registration(broker,listener,path,fds,count);
        struct MatonSessionReply response;
        assert(recv(client,&response,sizeof(response),0)==0);close(client);
        assert(broker->control_fd<0 && !broker->native_server);
    }
    assert(fchmod(native_fd,0777)==0);
    int client=registration(broker,listener,path,&native_fd,1);
    struct MatonSessionReply response;
    assert(recv(client,&response,sizeof(response),0)==sizeof(response));assert(response.status==0);
    assert(broker->native_server && broker->native_directory>=0);
    struct stat info;assert(lstat(bus,&info)==0 && S_ISSOCK(info.st_mode));
    assert((info.st_mode&0777)==0666);
    int connection=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(connection>=0);
    strcpy(address.sun_path,bus);assert(connect(connection,(struct sockaddr*)&address,sizeof(address))==0);close(connection);
    /* A repeat registration reuses the live portal without replacing the
     * directory capability; a not-yet-ready registration is unavailable. */
    int repeat=registration(broker,listener,path,&native_fd,1);
    assert(recv(repeat,&response,sizeof(response),0)==sizeof(response));assert(response.status==3);close(repeat);
    broker->ready_fd=-1;
    g_hash_table_insert(broker->owners,g_strdup("org.freedesktop.portal.Flatpak"),GUINT_TO_POINTER(1));
    int retained=broker->native_directory;
    repeat=registration(broker,listener,path,&native_fd,1);
    assert(recv(repeat,&response,sizeof(response),0)==sizeof(response));assert(response.status==2);
    assert(response.supervisor==getpid() && broker->native_directory==retained);close(repeat);
    close(client);
    guint source=broker->control_client_source;g_source_remove(source);
    assert(control_closed(broker->control_fd,G_IO_HUP,broker)==G_SOURCE_REMOVE);
    assert(broker->control_fd<0 && broker->native_directory<0 && !broker->native_server);
    assert(lstat(bus,&info)<0 && errno==ENOENT);
    close(file);close(native_fd);close(listener);broker_free(broker);
    unlink(path);unlink(policy);rmdir(directory);
    puts("PASS: native listener requires one system-owned directory FD; invalid registrations fail closed and revocation removes the bus");
    return 0;
}
