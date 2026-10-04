/* Transport and GLib/Java codec interoperability without a pathname listener
 * or GSocket (both are forbidden in some managed test hosts). */
#include "portals.c"
#include <fcntl.h>
#include <sys/wait.h>

static void read_all(int fd,void* buffer,size_t size) {
    size_t offset=0;while(offset<size){ssize_t n=read(fd,(char*)buffer+offset,size-offset);g_assert_cmpint(n,>,0);offset+=n;}
}
static GDBusMessage* exchange(Backend* backend,int peer,int input,int output,GDBusMessage* call) {
    g_assert_true(send_frame(backend,call));guint32 header[2];read_all(peer,header,sizeof(header));
    guint32 size=GUINT32_FROM_LE(header[0]);g_assert_cmpuint(GUINT32_FROM_LE(header[1]),==,0);
    guint8* blob=g_malloc(size);read_all(peer,blob,size);
    g_assert_cmpint(write(input,header,sizeof(header)),==,(ssize_t)sizeof(header));g_assert_cmpint(write(input,blob,size),==,size);g_free(blob);
    read_all(output,header,sizeof(header));size=GUINT32_FROM_LE(header[0]);g_assert_cmpuint(size,<=,MAX_FRAME);
    blob=g_malloc(size);read_all(output,blob,size);GError* error=NULL;
    GDBusMessage* reply=g_dbus_message_new_from_blob(blob,size,G_DBUS_CAPABILITY_FLAGS_NONE,&error);
    g_assert_no_error(error);g_free(blob);g_assert_nonnull(reply);
    g_assert_cmpuint(g_dbus_message_get_reply_serial(reply),==,g_dbus_message_get_serial(call));return reply;
}
static GDBusMessage* method(const char* iface,const char* name,GVariant* body,guint serial) {
    GDBusMessage* m=g_dbus_message_new_method_call("org.freedesktop.portal.Desktop","/org/freedesktop/portal/desktop",iface,name);
    g_dbus_message_set_sender(m,":1.42");g_dbus_message_set_serial(m,serial);g_dbus_message_set_body(m,body);return m;
}
int main(void) {
    const char* classes=g_getenv("MATON_PORTAL_TEST_CLASSPATH");g_assert_nonnull(classes);
    int pair[2];g_assert_cmpint(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair),==,0);
    Backend backend={.fd=pair[0]};GError* error=NULL;GPid child;int input,output;
    char* argv[]={"java","-cp",(char*)classes,"org.matonos.compositor.PortalTestPeer",NULL};
    g_assert_true(g_spawn_async_with_pipes(NULL,argv,NULL,G_SPAWN_SEARCH_PATH|G_SPAWN_DO_NOT_REAP_CHILD,NULL,NULL,&child,&input,&output,NULL,&error));g_assert_no_error(error);
    for(unsigned endian=0;endian<2;endian++) {
        GDBusMessage* call=method("org.freedesktop.portal.Settings","Read",g_variant_new("(ss)","org.freedesktop.appearance","color-scheme"),1+endian);
        g_dbus_message_set_byte_order(call,endian?G_DBUS_MESSAGE_BYTE_ORDER_BIG_ENDIAN:G_DBUS_MESSAGE_BYTE_ORDER_LITTLE_ENDIAN);
        GDBusMessage* reply=exchange(&backend,pair[1],input,output,call);
        GVariant *outer,*inner;g_variant_get(g_dbus_message_get_body(reply),"(v)",&outer);
        g_assert_true(g_variant_is_of_type(outer,G_VARIANT_TYPE_VARIANT));inner=g_variant_get_variant(outer);
        g_assert_cmpuint(g_variant_get_uint32(inner),==,1);g_variant_unref(inner);g_variant_unref(outer);g_object_unref(reply);g_object_unref(call);
    }
    GDBusMessage* call=method("org.freedesktop.portal.Settings","ReadAll",g_variant_new("(^as)",(char*[]){"org.*",NULL}),3);
    GDBusMessage* reply=exchange(&backend,pair[1],input,output,call);
    GVariant* dict=g_variant_get_child_value(g_dbus_message_get_body(reply),0);g_assert_cmpuint(g_variant_n_children(dict),==,2);
    GVariant *appearance=g_variant_lookup_value(dict,"org.freedesktop.appearance",G_VARIANT_TYPE_VARDICT);
    double r,g,b;g_assert_true(g_variant_lookup(appearance,"accent-color","(ddd)",&r,&g,&b));g_assert_cmpfloat(r,==,0.1);g_assert_cmpfloat(g,==,0.2);g_assert_cmpfloat(b,==,0.3);
    g_variant_unref(appearance);g_variant_unref(dict);g_object_unref(call);g_object_unref(reply);
    call=method("org.freedesktop.portal.Unknown","Nope",NULL,4);reply=exchange(&backend,pair[1],input,output,call);
    g_assert_cmpstr(g_dbus_message_get_error_name(reply),==,"org.freedesktop.DBus.Error.UnknownMethod");g_object_unref(call);g_object_unref(reply);
    close(input);close(output);waitpid(child,NULL,0);g_spawn_close_pid(child);
    /* SCM_RIGHTS preserves the file capability and never sends a pathname. */
    char filename[]="/tmp/maton-portal-fd-XXXXXX";int file=mkstemp(filename);g_assert_cmpint(file,>=,0);unlink(filename);
    g_assert_cmpint(write(file,"capability",10),==,10);g_assert_cmpint(lseek(file,0,SEEK_SET),==,0);
    GUnixFDList* list=g_unix_fd_list_new();g_assert_cmpint(g_unix_fd_list_append(list,file,&error),==,0);g_assert_no_error(error);
    call=method("org.freedesktop.portal.OpenURI","OpenFile",g_variant_new("(sha{sv})","",0,NULL),5);
    g_dbus_message_set_unix_fd_list(call,list);g_dbus_message_set_num_unix_fds(call,1);g_object_unref(list);close(file);
    g_assert_true(send_frame(&backend,call));guint32 header[2];struct iovec vec={header,sizeof(header)};
    char rights[CMSG_SPACE(sizeof(int))];struct msghdr msg={.msg_iov=&vec,.msg_iovlen=1,.msg_control=rights,.msg_controllen=sizeof(rights)};
    g_assert_cmpint(recvmsg(pair[1],&msg,MSG_CMSG_CLOEXEC),==,8);g_assert_cmpuint(GUINT32_FROM_LE(header[1]),==,1);
    struct cmsghdr* c=CMSG_FIRSTHDR(&msg);g_assert_nonnull(c);g_assert_cmpint(c->cmsg_type,==,SCM_RIGHTS);
    int received;memcpy(&received,CMSG_DATA(c),sizeof(received));char payload[10];read_all(received,payload,sizeof(payload));g_assert_cmpmem(payload,10,"capability",10);
    g_assert_true(fcntl(received,F_GETFD)&FD_CLOEXEC);close(received);
    guint32 size=GUINT32_FROM_LE(header[0]);guint8* blob=g_malloc(size);read_all(pair[1],blob,size);
    reply=g_dbus_message_new_from_blob(blob,size,G_DBUS_CAPABILITY_FLAGS_UNIX_FD_PASSING,&error);g_assert_no_error(error);
    g_assert_cmpuint(g_dbus_message_get_num_unix_fds(reply),==,1);g_free(blob);g_object_unref(reply);g_object_unref(call);close(pair[0]);close(pair[1]);
    /* Fragmented stream frames are retained; malformed descriptor counts
     * and oversize headers close the transport without allocating a body. */
    g_assert_cmpint(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair),==,0);
    Backend parser={.fd=pair[0],.input=g_byte_array_new(),.pending=g_hash_table_new(g_str_hash,g_str_equal)};
    call=method("org.freedesktop.portal.Settings","ReadOne",g_variant_new("(ss)","org.freedesktop.appearance","color-scheme"),10);
    reply=g_dbus_message_new_method_reply(call);g_dbus_message_set_serial(reply,100);
    g_dbus_message_set_body(reply,g_variant_new("(v)",g_variant_new_uint32(1)));
    gsize blob_size;blob=g_dbus_message_to_blob(reply,&blob_size,G_DBUS_CAPABILITY_FLAGS_NONE,&error);g_assert_no_error(error);
    header[0]=GUINT32_TO_LE(blob_size);header[1]=0;
    g_assert_cmpint(write(pair[1],header,3),==,3);g_assert_true(receive_frame(pair[0],G_IO_IN,&parser));g_assert_cmpuint(parser.input->len,==,3);
    g_assert_cmpint(write(pair[1],((char*)header)+3,5),==,5);g_assert_cmpint(write(pair[1],blob,blob_size),==,(ssize_t)blob_size);
    g_assert_true(receive_frame(pair[0],G_IO_IN,&parser));g_assert_cmpuint(parser.input->len,==,0);g_assert_cmpint(parser.fd,>=,0);
    header[1]=GUINT32_TO_LE(1);g_assert_cmpint(write(pair[1],header,8),==,8);
    g_assert_false(receive_frame(pair[0],G_IO_IN,&parser));g_assert_cmpint(parser.fd,==,-1);
    g_free(blob);g_object_unref(call);g_object_unref(reply);g_byte_array_unref(parser.input);g_hash_table_unref(parser.pending);close(pair[1]);
    g_print("PASS: GLib/Java little/big-endian messages, nested Settings variants/dictionaries/accent tuple, Java errors and SCM_RIGHTS capability with CLOEXEC; fragmented receive and malformed frames\n");return 0;
}
