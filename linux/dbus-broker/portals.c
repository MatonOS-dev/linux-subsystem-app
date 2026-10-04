#include "broker.h"
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* MBP1: eight-byte LE length/fd-count followed by one D-Bus wire message.
 * SCM_RIGHTS accompanies the first byte. Only the host receives descriptors.
 * All channel state is owned by the default main context, never the GDBus
 * filter worker. The broker authenticates/authorizes before scheduling here. */
#define MAX_FRAME (1024u*1024u)
#define MAX_FDS 16
#define MAX_CALLS 256

typedef struct {
    GDBusConnection* connection;
    GDBusMessage* call;
    gint64 deadline;
} Call;
typedef struct {
    Broker* broker;
    int fd;
    guint source, timer;
    GByteArray* input;
    GHashTable* pending;
    guint serial;
    gint refs, stopped, queued;
} Backend;
typedef struct {
    Backend* backend;
    GDBusConnection* connection;
    GDBusMessage* message;
} Forward;
static void backend_unref(Backend* p) {
    if(g_atomic_int_dec_and_test(&p->refs)) {
        g_hash_table_unref(p->pending);g_byte_array_unref(p->input);g_free(p);
    }
}
static void call_free(void* data) {
    Call* c=data;g_object_unref(c->connection);g_object_unref(c->call);g_free(c);
}
static char* key(const char* owner,guint serial) {return g_strdup_printf("%s/%u",owner,serial);}
static void fail(Call* c,const char* reason) {
    if(g_dbus_message_get_flags(c->call)&G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED)return;
    GDBusMessage* error=g_dbus_message_new_method_error_literal(c->call,"org.freedesktop.portal.Error.Failed",reason);
    g_dbus_message_set_sender(error,":1.0");
    g_dbus_connection_send_message(c->connection,error,G_DBUS_SEND_MESSAGE_FLAGS_NONE,NULL,NULL);g_object_unref(error);
}
static void unavailable(Backend* p) {
    if(p->fd>=0){shutdown(p->fd,SHUT_RDWR);close(p->fd);p->fd=-1;}
    if(p->source){g_source_remove(p->source);p->source=0;}
    GHashTableIter iter;gpointer value;g_hash_table_iter_init(&iter,p->pending);
    while(g_hash_table_iter_next(&iter,NULL,&value))fail(value,"Java portal backend disconnected");
    g_hash_table_remove_all(p->pending);
}
static gboolean send_frame(Backend* p,GDBusMessage* m) {
    if(p->fd<0)return FALSE;
    GError* error=NULL;gsize size=0;
    GUnixFDList* list=g_dbus_message_get_unix_fd_list(m);
    int count=list?g_unix_fd_list_get_length(list):0;
    if(count>MAX_FDS)return FALSE;
    guint8* blob=g_dbus_message_to_blob(m,&size,G_DBUS_CAPABILITY_FLAGS_UNIX_FD_PASSING,&error);
    if(!blob || size>MAX_FRAME){g_free(blob);g_clear_error(&error);return FALSE;}
    guint32 header[2]={GUINT32_TO_LE((guint32)size),GUINT32_TO_LE((guint32)count)};
    struct iovec vectors[2]={{header,sizeof(header)},{blob,size}};
    char control[CMSG_SPACE(sizeof(int)*MAX_FDS)];
    struct msghdr msg={.msg_iov=vectors,.msg_iovlen=2};
    if(count) {
        msg.msg_control=control;msg.msg_controllen=CMSG_SPACE(sizeof(int)*count);
        memset(control,0,msg.msg_controllen);struct cmsghdr* c=CMSG_FIRSTHDR(&msg);
        c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int)*count);
        memcpy(CMSG_DATA(c),g_unix_fd_list_peek_fds(list,NULL),sizeof(int)*count);
    }
    /* A bounded send prevents a stalled host from wedging bus routing. */
    ssize_t n;do {n=sendmsg(p->fd,&msg,MSG_NOSIGNAL);}while(n<0&&errno==EINTR);
    gsize total=sizeof(header)+size;
    if(n>0 && (gsize)n<total) {
        guint8* frame=g_malloc(total);memcpy(frame,header,sizeof(header));memcpy(frame+sizeof(header),blob,size);
        gsize offset=n;
        while(offset<total) {do {n=send(p->fd,frame+offset,total-offset,MSG_NOSIGNAL);}while(n<0&&errno==EINTR);if(n<=0)break;offset+=n;}
        g_free(frame);n=offset==total?(ssize_t)total:-1;
    }
    g_free(blob);return n==(ssize_t)total;
}
static gboolean receive_frame(int fd,GIOCondition condition,gpointer data) {
    Backend* p=data;
    if(condition&(G_IO_ERR|G_IO_NVAL)){p->source=0;unavailable(p);return G_SOURCE_REMOVE;}
    guint8 buffer[8192];ssize_t n=recv(fd,buffer,sizeof(buffer),MSG_DONTWAIT);
    if(n<0&&(errno==EAGAIN||errno==EINTR))return G_SOURCE_CONTINUE;
    if(n<=0){p->source=0;unavailable(p);return G_SOURCE_REMOVE;}
    g_byte_array_append(p->input,buffer,n);
    while(p->input->len>=8) {
        guint32 length,fds;memcpy(&length,p->input->data,4);memcpy(&fds,p->input->data+4,4);
        length=GUINT32_FROM_LE(length);fds=GUINT32_FROM_LE(fds);
        if(length<16||length>MAX_FRAME||fds){p->source=0;unavailable(p);return G_SOURCE_REMOVE;}
        if(p->input->len<length+8)break;
        GError* error=NULL;GDBusMessage* m=g_dbus_message_new_from_blob(p->input->data+8,length,G_DBUS_CAPABILITY_FLAGS_NONE,&error);
        if(!m){g_clear_error(&error);p->source=0;unavailable(p);return G_SOURCE_REMOVE;}
        const char* dest=g_dbus_message_get_destination(m);GDBusMessageType type=g_dbus_message_get_message_type(m);
        if(dest && (type==G_DBUS_MESSAGE_TYPE_METHOD_RETURN||type==G_DBUS_MESSAGE_TYPE_ERROR)) {
            char* k=key(dest,g_dbus_message_get_reply_serial(m));Call* c=g_hash_table_lookup(p->pending,k);
            if(c) {
                g_dbus_message_set_sender(m,":1.0");
                g_dbus_connection_send_message(c->connection,m,G_DBUS_SEND_MESSAGE_FLAGS_NONE,NULL,NULL);
                g_hash_table_remove(p->pending,k);
            }
            g_free(k);
        } else if(dest && type==G_DBUS_MESSAGE_TYPE_SIGNAL)broker_portal_signal(p->broker,m);
        g_object_unref(m);g_byte_array_remove_range(p->input,0,length+8);
    }
    return G_SOURCE_CONTINUE;
}
static gboolean timeout_calls(gpointer data) {
    Backend* p=data;GHashTableIter iter;gpointer value;g_hash_table_iter_init(&iter,p->pending);
    while(g_hash_table_iter_next(&iter,NULL,&value)) {
        Call* c=value;if(c->deadline<g_get_monotonic_time()){unavailable(p);break;}
    }
    return G_SOURCE_CONTINUE;
}
static gboolean forward_call(gpointer data) {
    Forward* f=data;Backend* p=f->backend;
    g_atomic_int_add(&p->queued,-1);
    Call* c=g_new0(Call,1);c->connection=f->connection;c->call=f->message;c->deadline=g_get_monotonic_time()+15*G_TIME_SPAN_SECOND;
    const char* live=g_atomic_int_get(&p->stopped)?NULL:broker_connection_name(p->broker,f->connection);
    if(!live || g_dbus_connection_is_closed(f->connection) || g_strcmp0(live,g_dbus_message_get_sender(f->message))!=0) {
        call_free(c);backend_unref(p);g_free(f);return G_SOURCE_REMOVE;
    }
    if(p->fd<0 || g_hash_table_size(p->pending)>=MAX_CALLS) {fail(c,"Java portal backend unavailable or busy");call_free(c);}
    else {
        char* k=key(g_dbus_message_get_sender(c->call),g_dbus_message_get_serial(c->call));
        if(g_hash_table_contains(p->pending,k)){fail(c,"Duplicate pending serial");call_free(c);g_free(k);}
        else if(!send_frame(p,c->call)){fail(c,"Java portal transport failed");call_free(c);g_free(k);unavailable(p);}
        else if(g_dbus_message_get_flags(c->call)&G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED){call_free(c);g_free(k);}
        else g_hash_table_insert(p->pending,k,c);
    }
    backend_unref(p);g_free(f);return G_SOURCE_REMOVE;
}
gboolean broker_portal_forward(Broker* b,GDBusConnection* connection,GDBusMessage* message,const char* owner) {
    const char* path=g_dbus_message_get_path(message);
    if(g_strcmp0(g_dbus_message_get_interface(message),"org.freedesktop.DBus.Peer")==0)return FALSE;
    if(!broker_session_data(b)||!path || !(g_str_equal(path,"/org/freedesktop/portal/desktop") ||
            g_str_has_prefix(path,"/org/freedesktop/portal/desktop/")))return FALSE;
    Backend* p=broker_session_data(b);
    if(g_atomic_int_add(&p->queued,1)>=MAX_CALLS) {
        g_atomic_int_add(&p->queued,-1);
        GDBusMessage* reply=g_dbus_message_new_method_error_literal(message,"org.freedesktop.DBus.Error.LimitsExceeded","Java portal queue is full");
        if(!(g_dbus_message_get_flags(message)&G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED))
            g_dbus_connection_send_message(connection,reply,G_DBUS_SEND_MESSAGE_FLAGS_NONE,NULL,NULL);
        g_object_unref(reply);return TRUE;
    }
    g_atomic_int_inc(&p->refs);
    Forward* f=g_new0(Forward,1);f->backend=p;f->connection=g_object_ref(connection);f->message=g_dbus_message_copy(message,NULL);
    g_dbus_message_set_sender(f->message,owner);
    g_main_context_invoke(NULL,forward_call,f);return TRUE;
}
void broker_session_client_closed(Broker* b,GDBusConnection* c) {
    Backend* p=broker_session_data(b);if(!p)return;
    const char* owner=broker_connection_name(b,c);if(!owner)return;
    GHashTableIter iter;gpointer value;g_hash_table_iter_init(&iter,p->pending);
    while(g_hash_table_iter_next(&iter,NULL,&value))if(((Call*)value)->connection==c)g_hash_table_iter_remove(&iter);
    GDBusMessage* m=g_dbus_message_new_signal("/org/matonos/PortalBackend","org.matonos.PortalBackend","ClientClosed");
    g_dbus_message_set_serial(m,++p->serial);g_dbus_message_set_body(m,g_variant_new("(s)",owner));
    if(!send_frame(p,m))unavailable(p);
    g_object_unref(m);
}
void broker_session_services_free(Broker* b) {
    Backend* p=broker_session_data(b);if(!p)return;
    g_atomic_int_set(&p->stopped,1);
    unavailable(p);if(p->timer)g_source_remove(p->timer);
    broker_set_session_data(b,NULL);backend_unref(p);
}
static void helper_call(GDBusConnection* c,const char* sender,const char* path,const char* iface,const char* method,
        GVariant* args,GDBusMethodInvocation* inv,void* data) {
    (void)c;(void)sender;(void)path;(void)iface;(void)method;(void)args;
    const char* monitor=broker_monitor_path(data);
    if(!monitor[0]){g_dbus_method_invocation_return_error(inv,G_IO_ERROR,G_IO_ERROR_NOT_INITIALIZED,"Session portal is not ready");return;}
    GVariantBuilder result;g_variant_builder_init(&result,G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&result,"{sv}","path",g_variant_new_string(monitor));
    g_dbus_method_invocation_return_value(inv,g_variant_new("(a{sv})",&result));
}
static GVariant* helper_property(GDBusConnection* c,const char* sender,const char* path,const char* iface,const char* name,GError** error,void* data) {
    (void)c;(void)sender;(void)path;(void)iface;(void)error;(void)data;
    return g_str_equal(name,"version")?g_variant_new_uint32(1):NULL;
}
gboolean broker_register_session_services(Broker* b,const char* monitor,int backend_fd,GError** error) {
    broker_set_monitor_path(b,monitor);
    Backend* p=g_new0(Backend,1);p->broker=b;p->fd=backend_fd;p->refs=1;
    p->input=g_byte_array_new();p->pending=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,call_free);
    broker_set_session_data(b,p);
    if(backend_fd>=0) {
        struct timeval timeout={.tv_sec=1};setsockopt(backend_fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        p->source=g_unix_fd_add(backend_fd,G_IO_IN|G_IO_HUP|G_IO_ERR,receive_frame,p);
    }
    p->timer=g_timeout_add_seconds(1,timeout_calls,p);
    GDBusNodeInfo* node=g_dbus_node_info_new_for_xml(
        "<node><interface name='org.matonos.PortalBackend'/><interface name='org.freedesktop.Flatpak.SessionHelper'>"
        "<property name='version' type='u' access='read'/><method name='RequestSession'><arg type='a{sv}' direction='out'/></method></interface></node>",error);
    if(!node)return FALSE;
    static const GDBusInterfaceVTable marker={0},helper={.method_call=helper_call,.get_property=helper_property};
    gboolean ok=broker_add_service(b,"org.freedesktop.portal.Desktop","/org/freedesktop/portal/desktop",node->interfaces[0],&marker,NULL,error) &&
        broker_add_service(b,"org.freedesktop.Flatpak","/org/freedesktop/Flatpak/SessionHelper",node->interfaces[1],&helper,b,error);
    g_dbus_node_info_unref(node);return ok;
}
