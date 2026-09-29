#include <gio/gio.h>
#include <stdio.h>

static gboolean signal_seen;
static gboolean owner_changed_seen;
typedef struct { GMainLoop *loop; GVariant *reply; GError *error; } CallResult;
static const char service_xml[] =
    "<node><interface name='org.example.Echo'>"
    "<method name='Echo'><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "</interface></node>";

static void echo_method(GDBusConnection *c, const char *sender, const char *path,
                        const char *interface, const char *method,
                        GVariant *parameters, GDBusMethodInvocation *inv,
                        gpointer data) {
    (void)c; (void)sender; (void)path; (void)interface; (void)method; (void)data;
    const char *text; g_variant_get(parameters, "(&s)", &text);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", text));
}
static void signal_cb(GDBusConnection *c, const char *sender, const char *path,
                      const char *interface, const char *signal,
                      GVariant *parameters, gpointer data) {
    (void)c; (void)sender; (void)path; (void)interface; (void)signal; (void)parameters; (void)data;
    signal_seen = TRUE;
}
static GDBusMessage *owner_changed_filter(GDBusConnection *c, GDBusMessage *message,
                                          gboolean incoming, gpointer data) {
    (void)c; (void)data;
    if (incoming && g_dbus_message_get_message_type(message) == G_DBUS_MESSAGE_TYPE_SIGNAL &&
        g_strcmp0(g_dbus_message_get_interface(message), "org.freedesktop.DBus") == 0 &&
        g_strcmp0(g_dbus_message_get_member(message), "NameOwnerChanged") == 0) {
        GVariant *body = g_dbus_message_get_body(message);
        const char *name = NULL;
        if (body) g_variant_get_child(body, 0, "&s", &name);
        if (g_strcmp0(name, "org.example.Transient") == 0) owner_changed_seen = TRUE;
    }
    return message;
}
static void routed_call_done(GObject *source, GAsyncResult *result, gpointer data) {
    CallResult *call = data;
    call->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &call->error);
    g_main_loop_quit(call->loop);
}
static GDBusConnection *connect_bus(const char *address) {
    GError *error = NULL;
    GDBusConnection *c = g_dbus_connection_new_for_address_sync(address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    if (error) { g_printerr("connect: %s\n", error->message); g_error_free(error); }
    return c;
}
static gboolean bus_call(GDBusConnection *c, const char *method, const char *signature,
                         GVariant *args, GVariant **result, GError **error) {
    *result = g_dbus_connection_call_sync(c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", method, args, signature ? G_VARIANT_TYPE(signature) : NULL,
        G_DBUS_CALL_FLAGS_NONE, 3000, NULL, error);
    return *result != NULL;
}
static gboolean request_name(GDBusConnection *c, const char *name, GError **error) {
    GVariant *r = NULL; if (!bus_call(c, "RequestName", "(u)", g_variant_new("(su)", name, 0u), &r, error)) return FALSE;
    guint code; g_variant_get(r, "(u)", &code); g_variant_unref(r); return code == 1;
}
static gboolean require(gboolean condition, const char *what, GError **error) {
    if (condition) return TRUE;
    g_printerr("FAIL: %s%s%s\n", what, error && *error ? ": " : "", error && *error ? (*error)->message : "");
    return FALSE;
}
int main(int argc, char **argv) {
    if (argc > 2) { g_printerr("usage: broker-test [BUS_ADDRESS]\n"); return 2; }
    const char *address = argc == 2 ? argv[1] : g_getenv("DBUS_SESSION_BUS_ADDRESS");
    if (address == NULL || *address == '\0') { g_printerr("set DBUS_SESSION_BUS_ADDRESS or pass BUS_ADDRESS\n"); return 2; }
    GError *error = NULL; gboolean ok = TRUE;
    GDBusConnection *sender = connect_bus(address), *receiver = connect_bus(address);
    if (!sender || !receiver) return 1;
    ok &= require(request_name(sender, "org.example.Sender", &error), "sender RequestName", &error);
    g_clear_error(&error);
    ok &= require(request_name(receiver, "org.example.Receiver", &error), "receiver RequestName", &error);
    g_clear_error(&error);

    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(service_xml, &error);
    static const GDBusInterfaceVTable vtable = { .method_call = echo_method };
    guint registration = g_dbus_connection_register_object(receiver, "/org/example/Receiver",
        node->interfaces[0], &vtable, NULL, NULL, &error);
    ok &= require(registration != 0, "register routed method", &error); g_clear_error(&error);

    CallResult call = { .loop = g_main_loop_new(NULL, FALSE) };
    g_dbus_connection_call(sender, "org.example.Receiver", "/org/example/Receiver",
        "org.example.Echo", "Echo", g_variant_new("(s)", "route-ok"), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 3000, NULL, routed_call_done, &call);
    g_main_loop_run(call.loop);
    GVariant *reply = call.reply; error = call.error;
    g_main_loop_unref(call.loop);
    const char *echo = NULL;
    if (reply) { g_variant_get(reply, "(&s)", &echo); ok &= require(g_str_equal(echo, "route-ok"), "unicast method route", NULL); g_variant_unref(reply); }
    else ok &= require(FALSE, "unicast method route", &error);
    g_clear_error(&error);

    g_dbus_connection_signal_subscribe(receiver, "org.example.Sender", "org.example.Events",
        "Changed", "/org/example/Sender", NULL, G_DBUS_SIGNAL_FLAGS_NONE, signal_cb, NULL, NULL);
    g_dbus_connection_add_filter(receiver, owner_changed_filter, NULL, NULL);
    GVariant *match_reply = NULL;
    ok &= require(bus_call(receiver, "AddMatch", NULL, g_variant_new("(s)",
        "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',path='/org/freedesktop/DBus',arg0='org.example.Transient',arg1=''"),
        &match_reply, &error), "AddMatch arg1", &error);
    if (match_reply) g_variant_unref(match_reply);
    g_clear_error(&error);
    g_dbus_connection_flush_sync(receiver, NULL, &error); g_clear_error(&error);
    g_dbus_connection_emit_signal(sender, NULL, "/org/example/Sender", "org.example.Events",
        "Changed", g_variant_new("(s)", "signal-ok"), &error);
    ok &= require(error == NULL, "emit signal", &error); g_clear_error(&error);
    gint64 deadline = g_get_monotonic_time() + 2000000;
    while (!signal_seen && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, FALSE); g_usleep(1000);
    }
    ok &= require(signal_seen, "match-rule signal forwarding", NULL);

    GVariant *names = NULL;
    ok &= require(bus_call(sender, "ListNames", "(as)", NULL, &names, &error), "ListNames", &error);
    gboolean saw_sender = FALSE, saw_receiver = FALSE, saw_service = FALSE;
    if (names) {
        GVariant *array; g_variant_get(names, "(@as)", &array);
        GVariantIter iter; const char *name; g_variant_iter_init(&iter, array);
        while (g_variant_iter_next(&iter, "&s", &name)) {
            saw_sender |= g_str_equal(name, "org.example.Sender");
            saw_receiver |= g_str_equal(name, "org.example.Receiver");
            saw_service |= g_str_equal(name, "org.matonos.Test");
        }
        g_variant_unref(array); g_variant_unref(names);
    }
    ok &= require(saw_sender && saw_receiver && saw_service, "ListNames contents", NULL);
    g_clear_error(&error);
    GVariant *owner = NULL;
    ok &= require(bus_call(sender, "GetNameOwner", "(s)", g_variant_new("(s)", "org.example.Receiver"), &owner, &error), "GetNameOwner", &error);
    if (owner) g_variant_unref(owner);
    g_clear_error(&error);

    GVariant *has_owner = NULL;
    ok &= require(bus_call(sender, "NameHasOwner", "(b)", g_variant_new("(s)", "org.example.Receiver"), &has_owner, &error), "NameHasOwner", &error);
    if (has_owner) { gboolean value; g_variant_get(has_owner, "(b)", &value); ok &= require(value, "NameHasOwner result", NULL); g_variant_unref(has_owner); }
    g_clear_error(&error);
    GVariant *name_result = NULL;
    ok &= require(bus_call(sender, "RequestName", "(u)", g_variant_new("(su)", "org.example.Transient", 0u), &name_result, &error), "RequestName", &error);
    if (name_result) { guint code; g_variant_get(name_result, "(u)", &code); ok &= require(code == 1, "RequestName result", NULL); g_variant_unref(name_result); }
    g_clear_error(&error);
    deadline = g_get_monotonic_time() + 1000000;
    while (!owner_changed_seen && g_get_monotonic_time() < deadline) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
    ok &= require(owner_changed_seen, "NameOwnerChanged match", NULL);
    ok &= require(bus_call(sender, "ReleaseName", "(u)", g_variant_new("(s)", "org.example.Transient"), &name_result, &error), "ReleaseName", &error);
    if (name_result) { guint code; g_variant_get(name_result, "(u)", &code); ok &= require(code == 1, "ReleaseName result", NULL); g_variant_unref(name_result); }
    g_clear_error(&error);

    reply = g_dbus_connection_call_sync(sender, "org.freedesktop.Flatpak", "/org/freedesktop/Flatpak",
        "org.freedesktop.Flatpak", "Call", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
    ok &= require(reply == NULL && error != NULL && g_dbus_error_is_remote_error(error), "Flatpak deny", NULL);
    g_clear_error(&error); if (reply) g_variant_unref(reply);
    GVariant *activation = NULL;
    ok &= require(!bus_call(sender, "StartServiceByName", "(u)",
        g_variant_new("(su)", "org.gtk.vfs.Daemon", 0u), &activation, &error) &&
        error != NULL && g_dbus_error_is_remote_error(error), "default-deny service activation", NULL);
    g_clear_error(&error); if (activation) g_variant_unref(activation);
    ok &= require(!request_name(sender, "org.example.Unlisted", &error), "default-deny RequestName", NULL);
    g_clear_error(&error);

    g_dbus_node_info_unref(node); g_object_unref(sender); g_object_unref(receiver);
    if (ok) g_print("broker host protocol checks passed\n");
    return ok ? 0 : 1;
}
