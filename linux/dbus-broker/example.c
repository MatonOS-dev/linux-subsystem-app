#include "broker.h"

static const char example_xml[] =
    "<node><interface name='org.matonos.Test'>"
    "<method name='Echo'><arg name='text' type='s' direction='in'/>"
    "<arg name='text' type='s' direction='out'/></method>"
    "</interface></node>";

static void example_method_call(GDBusConnection *connection, const char *sender,
                                const char *object_path, const char *interface,
                                const char *method, GVariant *parameters,
                                GDBusMethodInvocation *invocation,
                                gpointer user_data) {
    (void)connection; (void)sender; (void)object_path; (void)interface;
    (void)user_data;
    if (g_str_equal(method, "Echo")) {
        const char *text;
        g_variant_get(parameters, "(&s)", &text);
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(s)", text));
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "Unknown test method");
    }
}

gboolean broker_register_example(Broker *broker, GError **error) {
    static const GDBusInterfaceVTable vtable = {
        .method_call = example_method_call,
    };
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(example_xml, error);
    if (node == NULL) return FALSE;
    gboolean ok = broker_add_service(broker, "org.matonos.Test",
                                     "/org/matonos/Test", node->interfaces[0],
                                     &vtable, NULL, error);
    g_dbus_node_info_unref(node);
    return ok;
}
