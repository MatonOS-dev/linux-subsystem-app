#include "broker.h"
#include <string.h>

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
    return g_variant_new_uint32(g_str_equal(iface, "org.freedesktop.portal.Settings") ? 2 : 1);
}
static void call(GDBusConnection* c, const char* sender, const char* path,
                 const char* iface, const char* method, GVariant* args,
                 GDBusMethodInvocation* inv, void* data) {
    (void)c; (void)sender; (void)path; (void)iface;
    if (g_str_equal(method, "RequestSession")) {
        GVariantBuilder result; g_variant_builder_init(&result, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&result, "{sv}", "path", g_variant_new_string(data));
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
gboolean broker_register_session_services(Broker* broker, const char* monitor, GError** error) {
    static const GDBusInterfaceVTable table = { .method_call=call, .get_property=property };
    GDBusNodeInfo* node=g_dbus_node_info_new_for_xml(xml,error);
    if (!node) return FALSE;
    gboolean ok=broker_add_service(broker,"org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",node->interfaces[0],&table,NULL,error) &&
        broker_add_service(broker,"org.freedesktop.Flatpak",
            "/org/freedesktop/Flatpak/SessionHelper",node->interfaces[1],&table,(void*)monitor,error);
    g_dbus_node_info_unref(node);
    return ok;
}
