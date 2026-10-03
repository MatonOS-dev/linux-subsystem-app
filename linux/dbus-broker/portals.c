#include "broker.h"
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static const char xml[] =
    "<node><interface name='org.freedesktop.portal.Settings'>"
    "<property name='version' type='u' access='read'/>"
    "<method name='Read'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/></method>"
    "<method name='ReadOne'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/></method>"
    "<method name='ReadAll'><arg type='as' direction='in'/><arg type='a{sa{sv}}' direction='out'/></method>"
    "<signal name='SettingChanged'><arg type='s'/><arg type='s'/><arg type='v'/></signal>"
    "</interface><interface name='org.freedesktop.Flatpak.SessionHelper'>"
    "<property name='version' type='u' access='read'/>"
    "<method name='RequestSession'><arg type='a{sv}' direction='out'/></method>"
    "</interface></node>";

static GVariant* property(GDBusConnection* c, const char* sender, const char* path,
                         const char* iface, const char* name, GError** error, void* data) {
    (void)c; (void)sender; (void)path; (void)error; (void)data;
    if (!g_str_equal(name, "version")) return NULL;
    /* Settings v2 adds ReadOne; the session helper stays at 1. */
    return g_variant_new_uint32(g_str_equal(iface,"org.freedesktop.portal.Inhibit") ? 3 :
            (g_str_equal(iface, "org.freedesktop.portal.Settings") ? 2 : 1));
}
static void call(GDBusConnection* c, const char* sender, const char* path,
                 const char* iface, const char* method, GVariant* args,
                 GDBusMethodInvocation* inv, void* data) {
    (void)c; (void)sender; (void)path; (void)iface;
    if (g_str_equal(method, "RequestSession")) {
        const char* monitor=broker_monitor_path(data);
        if(!monitor[0]) {
            g_dbus_method_invocation_return_error(inv,G_IO_ERROR,G_IO_ERROR_NOT_INITIALIZED,"Session portal is not ready");return;
        }
        GVariantBuilder result; g_variant_builder_init(&result, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&result, "{sv}", "path", g_variant_new_string(monitor));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(a{sv})", &result));
    } else if (g_str_equal(method, "Read") || g_str_equal(method, "ReadOne")) {
        const char *ns, *key; g_variant_get(args, "(&s&s)", &ns, &key);
        GVariant* value=NULL;
        // No preference: applications use their own theme/contrast defaults.
        if (g_str_equal(ns,"org.freedesktop.appearance") &&
                (g_str_equal(key,"color-scheme") || g_str_equal(key,"contrast")))
            value=g_variant_new_uint32(0);
        // Android's caption bar owns minimize/maximize/close: toolkits draw none.
        // GTK prefers this over settings.ini whenever a settings portal answers.
        else if (g_str_equal(ns,"org.gnome.desktop.wm.preferences") && g_str_equal(key,"button-layout"))
            value=g_variant_new_string(":");
        if (!value) g_dbus_method_invocation_return_dbus_error(inv,
                "org.freedesktop.portal.Error.NotFound", "Setting is unavailable");
        // The deprecated Read wraps the value in a second variant; clients
        // unpack "(v)" and then "v" (GLib-CRITICAL on a bare value).
        else if (g_str_equal(method, "Read"))
            g_dbus_method_invocation_return_value(inv, g_variant_new("(v)",g_variant_new_variant(value)));
        else g_dbus_method_invocation_return_value(inv, g_variant_new("(v)",value));
    } else if (g_str_equal(method, "ReadAll")) {
        char** namespaces; g_variant_get(args,"(^as)",&namespaces);
        gboolean include = namespaces[0] == NULL, include_wm = include;
        for (unsigned i=0; namespaces[i]; ++i) {
            if (g_pattern_match_simple(namespaces[i],"org.freedesktop.appearance")) include=TRUE;
            if (g_pattern_match_simple(namespaces[i],"org.gnome.desktop.wm.preferences")) include_wm=TRUE;
        }
        GVariantBuilder result; g_variant_builder_init(&result,G_VARIANT_TYPE("a{sa{sv}}"));
        if (include) {
            GVariantBuilder appearance; g_variant_builder_init(&appearance,G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&appearance,"{sv}","color-scheme",g_variant_new_uint32(0));
            g_variant_builder_add(&appearance,"{sv}","contrast",g_variant_new_uint32(0));
            g_variant_builder_add(&result,"{sa{sv}}","org.freedesktop.appearance",&appearance);
        }
        if (include_wm) {
            GVariantBuilder wm; g_variant_builder_init(&wm,G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&wm,"{sv}","button-layout",g_variant_new_string(":"));
            g_variant_builder_add(&result,"{sa{sv}}","org.gnome.desktop.wm.preferences",&wm);
        }
        g_strfreev(namespaces);
        g_dbus_method_invocation_return_value(inv,g_variant_new("(a{sa{sv}})",&result));
    } else g_dbus_method_invocation_return_dbus_error(inv,
            "org.freedesktop.DBus.Error.UnknownMethod", "Unsupported portal operation");
}
/* Inhibit requests are registered only on the originating connection. This
 * makes Close and QueryEndResponse private even though all apps share a UID.
 * No logout/user-switch authority is claimed by this minimal Android backend. */
typedef struct PortalObject PortalObject;
typedef struct {
    Broker* broker;
    GPtrArray* objects;
    GDBusNodeInfo* node;
    int hold_fd;
    gboolean held;
    guint token;
} SessionPortals;
struct PortalObject {
    SessionPortals* portals;
    GDBusConnection* connection;
    char* owner;
    char* path;
    guint registration;
    guint idle;
    gboolean hold, session;
    PortalObject* monitor;
};
static const char inhibit_xml[] =
    "<node><interface name='org.freedesktop.portal.Inhibit'>"
    "<property name='version' type='u' access='read'/>"
    "<method name='Inhibit'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
    "<method name='CreateMonitor'><arg type='s' direction='in'/><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
    "<method name='QueryEndResponse'><arg type='o' direction='in'/></method>"
    "<signal name='StateChanged'><arg type='o'/><arg type='a{sv}'/></signal>"
    "</interface><interface name='org.freedesktop.portal.Request'>"
    "<method name='Close'/><signal name='Response'><arg type='u'/><arg type='a{sv}'/></signal>"
    "</interface><interface name='org.freedesktop.portal.Session'>"
    "<method name='Close'/><signal name='Closed'><arg type='a{sv}'/></signal>"
    "<property name='version' type='u' access='read'/></interface></node>";

/* ACK is sent only after Android acquires/releases its lock. Failure closes
 * the transport (EOF releases the Android hold) and fails this request. */
static gboolean android_hold(SessionPortals* p, gboolean held) {
    if(p->held==held)return TRUE;
    unsigned char value=held ? 1 : 0, ack=255;
    struct pollfd ready={.fd=p->hold_fd,.events=POLLIN};
    ssize_t n;
    do { n=send(p->hold_fd,&value,1,MSG_NOSIGNAL); } while(n<0 && errno==EINTR);
    int rc;
    rc=0;
    if(n==1)do { rc=poll(&ready,1,1500); } while(rc<0 && errno==EINTR);
    if(n==1 && rc>0 && recv(p->hold_fd,&ack,1,MSG_DONTWAIT)==1 && ack==value) {
        p->held=held;return TRUE;
    }
    if(p->hold_fd>=0)close(p->hold_fd);
    p->hold_fd=-1;p->held=FALSE;return FALSE;
}
static void update_hold(SessionPortals* p) {
    gboolean held=FALSE;
    for(guint i=0;i<p->objects->len;i++)held |= ((PortalObject*)g_ptr_array_index(p->objects,i))->hold;
    if(!android_hold(p,held))g_warning("Android inhibition transport unavailable");
}
static void object_destroy(void* data) {
    PortalObject* o=data;
    g_object_unref(o->connection);g_free(o->owner);g_free(o->path);g_free(o);
}
static void object_free(PortalObject* o) {
    if(!o->registration)return;
    if(o->idle){g_source_remove(o->idle);o->idle=0;}
    g_ptr_array_remove(o->portals->objects,o);
    guint registration=o->registration;o->registration=0;
    /* GDBus keeps user data alive until already queued callbacks retire. */
    g_dbus_connection_unregister_object(o->connection,registration);
}
static void object_close(GDBusConnection* c, const char* sender, const char* path,
        const char* iface, const char* method, GVariant* args, GDBusMethodInvocation* inv, void* data) {
    (void)c;(void)sender;(void)path;(void)iface;(void)method;(void)args;
    PortalObject* o=data;
    if(!o->registration) {
        g_dbus_method_invocation_return_error(inv,G_DBUS_ERROR,G_DBUS_ERROR_UNKNOWN_OBJECT,"Portal object is closed");return;
    }
    SessionPortals* p=o->portals;
    g_dbus_method_invocation_return_value(inv,NULL);
    if(o->monitor)object_free(o->monitor);
    /* A session may be closed before its pending CreateMonitor response. */
    for(guint i=0;i<p->objects->len;i++) {
        PortalObject* request=g_ptr_array_index(p->objects,i);
        if(request->monitor==o)request->monitor=NULL;
    }
    object_free(o);update_hold(p);
}
static void portal_signal(PortalObject* o,const char* path,const char* iface,const char* name,GVariant* body) {
    GDBusMessage* m=g_dbus_message_new_signal(path,iface,name);
    g_dbus_message_set_sender(m,":1.0");
    g_dbus_message_set_destination(m,o->owner);g_dbus_message_set_body(m,body);
    g_dbus_connection_send_message(o->connection,m,G_DBUS_SEND_MESSAGE_FLAGS_NONE,NULL,NULL);
    g_object_unref(m);
}
static gboolean request_response(void* data) {
    PortalObject* request=data;request->idle=0;
    GVariantBuilder result;g_variant_builder_init(&result,G_VARIANT_TYPE_VARDICT);
    if(request->monitor)g_variant_builder_add(&result,"{sv}","session_handle",g_variant_new_string(request->monitor->path));
    portal_signal(request,request->path,"org.freedesktop.portal.Request","Response",
            g_variant_new("(ua{sv})",(request->hold || request->monitor) ? 0u : 2u,&result));
    if(request->monitor) {
        GVariantBuilder state;g_variant_builder_init(&state,G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&state,"{sv}","screensaver-active",g_variant_new_boolean(FALSE));
        g_variant_builder_add(&state,"{sv}","session-state",g_variant_new_uint32(1));
        portal_signal(request,"/org/freedesktop/portal/desktop","org.freedesktop.portal.Inhibit","StateChanged",
                g_variant_new("(oa{sv})",request->monitor->path,&state));
    }
    if(!request->hold)object_free(request);
    return G_SOURCE_REMOVE;
}
static char* object_path(SessionPortals* p,const char* owner,const char* kind,GVariant* options,const char* key) {
    const char* token=NULL;
    GVariant* value=g_variant_lookup_value(options,key,NULL);
    if(value) {
        if(!g_variant_is_of_type(value,G_VARIANT_TYPE_STRING)){g_variant_unref(value);return NULL;}
        token=g_variant_get_string(value,NULL);
        if(!*token){g_variant_unref(value);return NULL;}
        for(const char* s=token;*s;s++)if(!g_ascii_isalnum(*s)&&*s!='_'){g_variant_unref(value);return NULL;}
    }
    char* sender=g_strdup(owner+1);
    for(char* s=sender;*s;s++)if(*s=='.')*s='_';
    char* path=token ? g_strdup_printf("/org/freedesktop/portal/desktop/%s/%s/%s",kind,sender,token) :
        g_strdup_printf("/org/freedesktop/portal/desktop/%s/%s/maton_%u",kind,sender,++p->token);
    g_free(sender);if(value)g_variant_unref(value);return path;
}
static PortalObject* object_new(SessionPortals* p,GDBusConnection* c,const char* owner,char* path,gboolean session,GError** error) {
    if(!path) {g_set_error_literal(error,G_DBUS_ERROR,G_DBUS_ERROR_INVALID_ARGS,"Invalid handle token");return NULL;}
    if(p->objects->len>=256) {g_free(path);g_set_error_literal(error,G_DBUS_ERROR,G_DBUS_ERROR_LIMITS_EXCEEDED,"Too many portal objects");return NULL;}
    for(guint i=0;i<p->objects->len;i++) {
        PortalObject* old=g_ptr_array_index(p->objects,i);
        if(!strcmp(path,old->path)){g_free(path);g_set_error_literal(error,G_DBUS_ERROR,G_DBUS_ERROR_INVALID_ARGS,"Handle token already in use");return NULL;}
    }
    PortalObject* o=g_new0(PortalObject,1);o->portals=p;o->connection=g_object_ref(c);
    o->owner=g_strdup(owner);o->path=path;o->session=session;
    static const GDBusInterfaceVTable table={.method_call=object_close,.get_property=property};
    o->registration=g_dbus_connection_register_object(c,path,p->node->interfaces[session?2:1],&table,o,object_destroy,error);
    if(!o->registration){g_object_unref(c);g_free(o->owner);g_free(path);g_free(o);return NULL;}
    g_ptr_array_add(p->objects,o);return o;
}
static void inhibit_call(GDBusConnection* c,const char* sender,const char* path,const char* iface,
        const char* method,GVariant* args,GDBusMethodInvocation* inv,void* data) {
    (void)sender;(void)path;(void)iface;
    SessionPortals* p=data;
    const char* owner=broker_connection_name(p->broker,c);
    if(!owner){g_dbus_method_invocation_return_error(inv,G_DBUS_ERROR,G_DBUS_ERROR_ACCESS_DENIED,"Hello required");return;}
    if(!strcmp(method,"QueryEndResponse")) {
        const char* handle;g_variant_get(args,"(&o)",&handle);
        for(guint i=0;i<p->objects->len;i++) {
            PortalObject* o=g_ptr_array_index(p->objects,i);
            if(o->session && o->connection==c && !strcmp(o->path,handle)) {
                /* Minimal monitor has no Android end-session source yet. */
                g_dbus_method_invocation_return_value(inv,NULL);return;
            }
        }
        g_dbus_method_invocation_return_error(inv,G_DBUS_ERROR,G_DBUS_ERROR_ACCESS_DENIED,"Unknown monitoring session");return;
    }
    gboolean monitor=!strcmp(method,"CreateMonitor");
    const char* window;guint flags=0;GVariant* options;
    if(monitor)g_variant_get(args,"(&s@a{sv})",&window,&options);
    else g_variant_get(args,"(&su@a{sv})",&window,&flags,&options);
    (void)window;
    /* Logout/user switch have no Android equivalent; never claim them. */
    if(!monitor && (!flags || (flags&~12u))) {
        g_variant_unref(options);
        g_dbus_method_invocation_return_error(inv,G_DBUS_ERROR,G_DBUS_ERROR_NOT_SUPPORTED,"Only suspend and idle inhibition are supported");return;
    }
    GError* error=NULL;
    PortalObject* request=object_new(p,c,owner,object_path(p,owner,"request",options,"handle_token"),FALSE,&error);
    if(request && monitor)request->monitor=object_new(p,c,owner,object_path(p,owner,"session",options,"session_handle_token"),TRUE,&error);
    g_variant_unref(options);
    if(request && !monitor && !android_hold(p,TRUE)) {
        g_set_error_literal(&error,G_IO_ERROR,G_IO_ERROR_FAILED,"Android inhibition transport unavailable");
    }
    if(error) {
        if(request)object_free(request);
        g_dbus_method_invocation_return_gerror(inv,error);g_error_free(error);return;
    }
    request->hold=!monitor;
    g_dbus_method_invocation_return_value(inv,g_variant_new("(o)",request->path));
    /* Both methods signal acceptance. Inhibit keeps its Request alive after
     * Response so Close still releases the hold; CreateMonitor retires it. */
    request->idle=g_idle_add(request_response,request);
}
void broker_session_client_closed(Broker* b,GDBusConnection* c) {
    SessionPortals* p=broker_session_data(b);if(!p)return;
    for(guint i=p->objects->len;i>0;i--) {
        PortalObject* o=g_ptr_array_index(p->objects,i-1);
        if(o->connection==c)object_free(o);
    }
    update_hold(p);
}
void broker_session_services_free(Broker* b) {
    SessionPortals* p=broker_session_data(b);if(!p)return;
    while(p->objects->len)object_free(g_ptr_array_index(p->objects,p->objects->len-1));
    if(p->hold_fd>=0)close(p->hold_fd);
    g_ptr_array_unref(p->objects);if(p->node)g_dbus_node_info_unref(p->node);g_free(p);
    broker_set_session_data(b,NULL);
}

gboolean broker_register_session_services(Broker* broker, const char* monitor, int hold_fd, GError** error) {
    broker_set_monitor_path(broker,monitor);
    SessionPortals* p=g_new0(SessionPortals,1);p->broker=broker;p->hold_fd=hold_fd;
    p->objects=g_ptr_array_new();p->node=g_dbus_node_info_new_for_xml(inhibit_xml,error);
    broker_set_session_data(broker,p);
    if(!p->node)return FALSE;
    static const GDBusInterfaceVTable table = { .method_call=call, .get_property=property };
    GDBusNodeInfo* node=g_dbus_node_info_new_for_xml(xml,error);
    if (!node) return FALSE;
    gboolean ok=broker_add_service(broker,"org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",node->interfaces[0],&table,NULL,error) &&
        broker_add_service(broker,"org.freedesktop.Flatpak",
            "/org/freedesktop/Flatpak/SessionHelper",node->interfaces[1],&table,broker,error);
    static const GDBusInterfaceVTable inhibit_table={.method_call=inhibit_call,.get_property=property};
    if(ok)ok=broker_add_service(broker,"org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",p->node->interfaces[0],&inhibit_table,p,error);
    g_dbus_node_info_unref(node);
    return ok;
}
