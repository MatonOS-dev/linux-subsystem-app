#include <gio/gio.h>
#include <stdio.h>
#include <unistd.h>

static GDBusConnection* connection;
static gboolean ok=TRUE;
static GVariant* call(const char* name,const char* path,const char* interface,
                       const char* method,GVariant* parameters,const char* type) {
    GError* error=NULL;
    GVariant* reply=g_dbus_connection_call_sync(connection,name,path,interface,method,
        parameters,type ? G_VARIANT_TYPE(type) : NULL,G_DBUS_CALL_FLAGS_NONE,5000,NULL,&error);
    if(!reply){g_printerr("FAIL %s.%s: %s\n",interface,method,error ? error->message : "no reply");ok=FALSE;}
    g_clear_error(&error);return reply;
}
static gboolean denied(const char* name,const char* path,const char* interface,
                        const char* method,GVariant* parameters) {
    GError* error=NULL;
    GVariant* reply=g_dbus_connection_call_sync(connection,name,path,interface,method,
        parameters,NULL,G_DBUS_CALL_FLAGS_NONE,5000,NULL,&error);
    gboolean rejected=reply==NULL && error && g_dbus_error_is_remote_error(error);
    if(reply)g_variant_unref(reply);
    g_clear_error(&error);return rejected;
}
#define CHECK(value,what) do {if(!(value)){g_printerr("FAIL %s\n",what);ok=FALSE;}} while(0)
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    GError* error=NULL;
    connection=g_dbus_connection_new_for_address_sync(argv[1],
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL,NULL,&error);
    if(!connection){g_printerr("connect: %s\n",error->message);g_error_free(error);return 1;}
    GVariant* owner=call("org.freedesktop.DBus","/","org.freedesktop.DBus",
        "GetNameOwner",g_variant_new("(s)","org.freedesktop.portal.Flatpak"),"(s)");
    if(owner){const char* name;g_variant_get(owner,"(&s)",&name);
        CHECK(g_str_has_prefix(name,":1."),"proxy bootstrap bus path");g_variant_unref(owner);}
    GVariant* reply=call("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus",
        "GetConnectionCredentials",g_variant_new("(s)",g_dbus_connection_get_unique_name(connection)),"(a{sv})");
    if(reply){
        GVariant* values;g_variant_get(reply,"(@a{sv})",&values);guint uid=0,pid=0;
        CHECK(g_variant_lookup(values,"UnixUserID","u",&uid)&&uid==geteuid(),"kernel UID");
        CHECK(g_variant_lookup(values,"ProcessID","u",&pid)&&pid==(guint)getpid(),"kernel PID");
        g_variant_unref(values);g_variant_unref(reply);
    }
    reply=call("org.freedesktop.portal.Flatpak","/org/freedesktop/portal/Flatpak",
        "org.freedesktop.DBus.Properties","GetAll",g_variant_new("(s)","org.freedesktop.portal.Flatpak"),"(a{sv})");
    if(reply){
        GVariant* values;g_variant_get(reply,"(@a{sv})",&values);guint version=0;
        CHECK(g_variant_lookup(values,"version","u",&version)&&version>=4,"upstream spawn portal version");
        g_variant_unref(values);g_variant_unref(reply);
    }
    reply=call("org.freedesktop.portal.Desktop","/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings","Read",g_variant_new("(ss)","org.freedesktop.appearance","color-scheme"),"(v)");
    if(reply){GVariant* value;g_variant_get(reply,"(v)",&value);
        CHECK(g_variant_is_of_type(value,G_VARIANT_TYPE_UINT32)&&g_variant_get_uint32(value)==0,"no-preference color scheme");
        g_variant_unref(value);g_variant_unref(reply);}
    reply=call("org.freedesktop.portal.Desktop","/org/freedesktop/portal/desktop",
        "org.freedesktop.DBus.Properties","GetAll",g_variant_new("(s)","org.freedesktop.portal.Settings"),"(a{sv})");
    if(reply)g_variant_unref(reply);
    reply=call("org.freedesktop.Flatpak","/org/freedesktop/Flatpak/SessionHelper",
        "org.freedesktop.Flatpak.SessionHelper","RequestSession",NULL,"(a{sv})");
    if(reply){GVariant* values;g_variant_get(reply,"(@a{sv})",&values);const char* monitor=NULL;
        CHECK(g_variant_lookup(values,"path","&s",&monitor)&&g_str_has_suffix(monitor,"-monitor"),"monitor directory");
        if(monitor){char* path=g_build_filename(monitor,"resolv.conf",NULL);char* text=NULL;
            CHECK(g_file_get_contents(path,&text,NULL,NULL)&&g_str_has_prefix(text,"nameserver "),"DNS monitor content");
            g_free(text);g_free(path);}
        g_variant_unref(values);g_variant_unref(reply);}
    CHECK(denied("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus",
        "RequestName",g_variant_new("(su)","org.freedesktop.portal.Flatpak",2u)),"portal impersonation denied");
    CHECK(denied("org.freedesktop.Flatpak","/org/freedesktop/Flatpak/Development",
        "org.freedesktop.Flatpak.Development","HostCommand",NULL),"host commands denied");
    const char* command[]={"/bin/true",NULL};
    CHECK(denied("org.freedesktop.portal.Flatpak","/org/freedesktop/portal/Flatpak",
        "org.freedesktop.portal.Flatpak","Spawn",
        g_variant_new("(^ay^aay@a{uh}@a{ss}u@a{sv})","/",command,
            g_variant_new_array(G_VARIANT_TYPE("{uh}"),NULL,0),
            g_variant_new_array(G_VARIANT_TYPE("{ss}"),NULL,0),0u,
            g_variant_new_array(G_VARIANT_TYPE("{sv}"),NULL,0))),"unsandboxed spawn rejected");
    g_object_unref(connection);
    if(ok)g_print("Session portal, DNS and impersonation checks passed\n");
    return ok ? 0 : 1;
}
