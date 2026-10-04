#include "broker.h"
/* Exercise the real accepted-connection path over authenticated socketpairs.
 * No pathname listener is needed in restricted host test environments. */
#include "broker.c"
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>

#include <sys/wait.h>
#include <unistd.h>

static guint responses, states, inhibited;
static char* session_handle;
static gpointer java_peer(gpointer data) {
    int fd=GPOINTER_TO_INT(data);GError* error=NULL;GPid child;int input,output;
    char* argv[]={"java","-cp",(char*)g_getenv("MATON_PORTAL_TEST_CLASSPATH"),"org.matonos.compositor.PortalTestPeer",NULL};
    g_assert_true(g_spawn_async_with_pipes(NULL,argv,NULL,G_SPAWN_SEARCH_PATH|G_SPAWN_DO_NOT_REAP_CHILD,NULL,NULL,&child,&input,&output,NULL,&error));
    g_assert_no_error(error);
    struct pollfd p[2]={{.fd=fd,.events=POLLIN},{.fd=output,.events=POLLIN}};
    for(;;) {
        if(poll(p,2,-1)<0)break;
        gboolean done=FALSE;
        for(unsigned i=0;i<2;i++)if(p[i].revents) {
            char bytes[8192];ssize_t n=read(p[i].fd,bytes,sizeof(bytes));
            if(n<=0){done=TRUE;break;}
            int target=i?fd:input;ssize_t offset=0;
            while(offset<n){ssize_t wrote=write(target,bytes+offset,n-offset);if(wrote<=0){done=TRUE;break;}offset+=wrote;}
        }
        if(done)break;
    }
    close(input);close(output);kill(child,SIGTERM);waitpid(child,NULL,0);g_spawn_close_pid(child);return NULL;
}
static void signal_cb(GDBusConnection* c,const char* sender,const char* path,const char* iface,
        const char* signal,GVariant* args,void* data) {
    (void)c;(void)sender;(void)path;(void)iface;(void)data;
    if(!strcmp(signal,"Response")) {
        guint code;GVariant* results;g_variant_get(args,"(u@a{sv})",&code,&results);
        g_assert_cmpuint(code,==,0);
        if(g_variant_lookup(results,"session_handle","s",&session_handle))responses++;else inhibited++;
        g_variant_unref(results);
    } else {
        const char* handle;GVariant* state;guint running=0;gboolean screensaver=TRUE;
        g_variant_get(args,"(&o@a{sv})",&handle,&state);
        g_assert_cmpstr(handle,==,session_handle);
        g_assert_true(g_variant_lookup(state,"session-state","u",&running));g_assert_cmpuint(running,==,1);
        g_assert_true(g_variant_lookup(state,"screensaver-active","b",&screensaver));g_assert_false(screensaver);
        g_variant_unref(state);states++;
    }
}
static GVariant* call(GDBusConnection* c,const char* path,const char* iface,const char* method,GVariant* args,gboolean success) {
    GError* error=NULL;
    GVariant* reply=g_dbus_connection_call_sync(c,"org.freedesktop.portal.Desktop",path,iface,method,args,NULL,
            G_DBUS_CALL_FLAGS_NONE,3000,NULL,&error);
    if(success && error)g_error("%s: %s",method,error->message);
    g_assert_cmpint(reply!=NULL,==,success);g_clear_error(&error);return reply;
}
static GVariant* options(const char* token) {
    GVariantBuilder b;g_variant_builder_init(&b,G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b,"{sv}","handle_token",g_variant_new_string(token));
    g_variant_builder_add(&b,"{sv}","session_handle_token",g_variant_new_string("monitor"));
    return g_variant_builder_end(&b);
}
static char* inhibit(GDBusConnection* c,const char* token,guint flags,gboolean success) {
    GVariant* r=call(c,"/org/freedesktop/portal/desktop","org.freedesktop.portal.Inhibit","Inhibit",
            g_variant_new("(su@a{sv})","",flags,options(token)),success);
    char* path=NULL;if(r){g_variant_get(r,"(o)",&path);g_variant_unref(r);}return path;
}
static void close_request(GDBusConnection* c,const char* path,gboolean success) {
    GVariant* r=call(c,path,"org.freedesktop.portal.Request","Close",NULL,success);if(r)g_variant_unref(r);
}
typedef struct {Broker* broker;int fds[2];} TestServer;
static GIOStream* peer_stream(int fd) {
    GError* error=NULL;GSocket* socket=g_socket_new_from_fd(fd,&error);g_assert_no_error(error);
    GSocketConnection* stream=g_socket_connection_factory_create_connection(socket);g_object_unref(socket);
    return G_IO_STREAM(stream);
}
static gpointer test_server(gpointer data) {
    TestServer* server=data;
    for(unsigned i=0;i<2;i++) {
        GError* error=NULL;GIOStream* stream=peer_stream(server->fds[i]);char* guid=g_dbus_generate_guid();
        GDBusConnection* c=g_dbus_connection_new_sync(stream,guid,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER|G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING,NULL,NULL,&error);
        g_assert_no_error(error);g_free(guid);g_object_unref(stream);
        g_assert_true(on_new_connection(NULL,c,server->broker));
        g_dbus_connection_start_message_processing(c);g_object_unref(c);
    }
    return NULL;
}
static GDBusConnection* test_connect(int fd) {
    GError* error=NULL;GIOStream* stream=peer_stream(fd);
    GDBusConnection* c=g_dbus_connection_new_sync(stream,NULL,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,NULL,NULL,&error);
    g_assert_no_error(error);g_assert_nonnull(c);g_object_unref(stream);return c;
}
int main(void) {
    if(!g_getenv("MATON_PORTAL_TEST_CLASSPATH")){g_print("Run make check-inhibit for Java backend classpath\n");return 77;}
    GError* error=NULL;char* dir=g_dir_make_tmp("maton-inhibit-XXXXXX",&error);g_assert_no_error(error);
    char* socket_path=g_build_filename(dir,"bus",NULL);char* policy=g_build_filename(dir,"policy",NULL);
    g_assert_true(g_file_set_contents(policy,"talk org.freedesktop.portal.Desktop\n",-1,&error));g_assert_no_error(error);
    int fds[2];g_assert_cmpint(socketpair(AF_UNIX,SOCK_STREAM,0,fds),==,0);
    /* Some managed hosts deny the socket inspection GIO requires even for
     * socketpairs. Keep compilation available and report runtime skip. */
    GSocket* probe=g_socket_new_from_fd(dup(fds[0]),&error);
    if(!probe) {
        g_print("SKIP: host sandbox prevents GIO socket setup: %s\n",error->message);
        g_clear_error(&error);close(fds[0]);close(fds[1]);unlink(policy);rmdir(dir);
        g_free(socket_path);g_free(policy);g_free(dir);return 77;
    }
    g_object_unref(probe);
    int peers[2][2];
    for(unsigned i=0;i<2;i++)g_assert_cmpint(socketpair(AF_UNIX,SOCK_STREAM,0,peers[i]),==,0);
    pid_t pid=fork();g_assert_cmpint(pid,>=,0);
    if(pid==0) {
        close(fds[0]);Broker* b=broker_new(socket_path,policy,&error);
        if(!b || !broker_register_session_services(b,dir,fds[1],&error))_exit(1);
        for(unsigned i=0;i<2;i++)close(peers[i][0]);
        TestServer server={.broker=b,.fds={peers[0][1],peers[1][1]}};
        g_thread_new("bus-test-accept",test_server,&server);
        GMainLoop* loop=g_main_loop_new(NULL,FALSE);g_main_loop_run(loop);_exit(0);
    }
    close(fds[1]);GThread* thread=g_thread_new("java-test",java_peer,GINT_TO_POINTER(fds[0]));
    for(unsigned i=0;i<2;i++)close(peers[i][1]);
    GDBusConnection* a=test_connect(peers[0][0]);
    GDBusConnection* b=test_connect(peers[1][0]);
    GVariant* r=call(a,"/org/freedesktop/portal/desktop","org.freedesktop.DBus.Properties","Get",
            g_variant_new("(ss)","org.freedesktop.portal.Inhibit","version"),TRUE);
    GVariant* v;g_variant_get(r,"(v)",&v);g_assert_cmpuint(g_variant_get_uint32(v),==,3);g_variant_unref(v);g_variant_unref(r);
    guint sub=g_dbus_connection_signal_subscribe(a,"org.freedesktop.portal.Desktop",NULL,NULL,NULL,NULL,G_DBUS_SIGNAL_FLAGS_NONE,signal_cb,NULL,NULL);
    char* first=inhibit(a,"first",8,TRUE);
    char* expected=g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/first",g_dbus_connection_get_unique_name(a)+1);
    for(char* p=expected;*p;p++)if(*p=='.')*p='_';
    g_assert_cmpstr(first,==,expected);g_free(expected);
    close_request(b,first,FALSE);
    g_assert_null(inhibit(a,"first",4,FALSE)); /* duplicate */
    g_assert_null(inhibit(a,"bad/token",8,FALSE));
    g_assert_null(inhibit(a,"zero",0,FALSE));
    g_assert_null(inhibit(a,"logout",1,FALSE));
    g_assert_null(inhibit(a,"unknown",16,FALSE));
    char* second=inhibit(a,"second",4,TRUE);char* third=inhibit(b,"third",12,TRUE);
    close_request(a,first,TRUE);close_request(a,first,FALSE);close_request(a,second,TRUE);
    g_dbus_connection_close_sync(b,NULL,&error);g_assert_no_error(error);
    for(unsigned i=0;i<100 && inhibited<2;i++){while(g_main_context_iteration(NULL,FALSE)){}g_usleep(10000);}
    g_assert_cmpuint(inhibited,==,2);
    r=call(a,"/org/freedesktop/portal/desktop","org.freedesktop.portal.Inhibit","CreateMonitor",
            g_variant_new("(s@a{sv})","",options("create")),TRUE);
    char* request;g_variant_get(r,"(o)",&request);g_variant_unref(r);
    for(unsigned i=0;i<100 && (!responses || !states);i++){while(g_main_context_iteration(NULL,FALSE)){}g_usleep(10000);}
    g_assert_cmpuint(responses,==,1);g_assert_cmpuint(states,==,1);close_request(a,request,FALSE);
    r=call(a,"/org/freedesktop/portal/desktop","org.freedesktop.portal.Inhibit","QueryEndResponse",g_variant_new("(o)",session_handle),TRUE);g_variant_unref(r);
    r=call(a,session_handle,"org.freedesktop.portal.Session","Close",NULL,TRUE);g_variant_unref(r);
    call(a,"/org/freedesktop/portal/desktop","org.freedesktop.portal.Inhibit","QueryEndResponse",g_variant_new("(o)",session_handle),FALSE);
    g_dbus_connection_signal_unsubscribe(a,sub);
    /* Transport death must fail an inhibition instead of reporting a hold. */
    shutdown(fds[0],SHUT_RDWR);g_thread_join(thread);close(fds[0]);
    g_assert_null(inhibit(a,"broken",8,FALSE));
    g_dbus_connection_close_sync(a,NULL,NULL);g_object_unref(a);g_object_unref(b);
    kill(pid,SIGTERM);waitpid(pid,NULL,0);unlink(socket_path);unlink(policy);rmdir(dir);
    g_free(request);g_free(session_handle);g_free(first);g_free(second);g_free(third);
    g_free(socket_path);g_free(policy);g_free(dir);
    g_print("Inhibit aggregation, ownership, disconnect, monitor and transport failure checks passed\n");return 0;
}
