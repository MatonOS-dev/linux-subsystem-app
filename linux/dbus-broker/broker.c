#include "broker.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <glib-unix.h>
#include <poll.h>
#include <stdio.h>
#include <sys/syscall.h>
#include "session-control.h"
#include "flatpak-compat.h"

typedef struct _Client Client;
typedef struct _Service Service;
typedef struct _Pending Pending;

struct _Client {
    Broker *broker;
    GDBusConnection *connection;
    char *unique;
    GPtrArray *matches;
    GPtrArray *outgoing; /* Pending calls originated here, for disconnect cleanup. */
    GHashTable *pending; /* forwarded serial -> Pending */
    guint filter_id, closed_id;
    gint portal_generation;
};
struct _Service {
    char *name, *path;
    GDBusInterfaceInfo *interface_info;
    const GDBusInterfaceVTable *vtable;
    gpointer user_data;
};
struct _Pending { Client *origin; guint32 serial; };
struct _Broker {
    char *socket_path;
    uid_t owner_uid;
    GDBusServer *server;
    GMainLoop *loop;
    GPtrArray *clients;
    GPtrArray *services;
    GHashTable *owners; /* bus name -> Client* (NULL value for internal service) */
    GHashTable *name_flags; /* owned name -> RequestName flags */
    GHashTable *rules;  /* name -> access flags */
    guint next_id;
    char *id;
    gboolean trace_calls;
    void* session_services;
    gboolean flatpak_portal;
    GPid portal_pid;
    int portal_pidfd;
    int ready_fd;
    gboolean host_session;
    int control_listener, control_fd;
    guint control_source, control_client_source;
    char *control_path;
    char monitor[192];
    char flatpak_error[256];
    pid_t supervisor_pid;
    gint portal_generation;
};

enum { ACCESS_OWN = 1, ACCESS_TALK = 2 };
enum { MAX_CLIENTS = 1024, MAX_MATCHES_PER_CLIENT = 256, MAX_PENDING_PER_CLIENT = 256 };
static gboolean managed_portal_credentials(Broker* b,GCredentials* credentials,gint generation) {
    struct pollfd alive={.fd=b->portal_pidfd,.events=POLLIN};
    return generation==g_atomic_int_get(&b->portal_generation) && credentials && b->portal_pid>0 && b->portal_pidfd>=0 &&
        poll(&alive,1,0)==0 &&
        g_credentials_get_unix_user(credentials,NULL)==1000 &&
        g_credentials_get_unix_pid(credentials,NULL)==b->portal_pid;
}
static const char dbus_xml[] =
    "<node><interface name='org.freedesktop.DBus'>"
    "<method name='Hello'><arg type='s' direction='out'/></method>"
    "<method name='RequestName'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='ReleaseName'><arg type='s' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='GetNameOwner'><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='NameHasOwner'><arg type='s' direction='in'/><arg type='b' direction='out'/></method>"
    "<method name='ListNames'><arg type='as' direction='out'/></method>"
    "<method name='ListActivatableNames'><arg type='as' direction='out'/></method>"
    "<method name='StartServiceByName'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='AddMatch'><arg type='s' direction='in'/></method>"
    "<method name='RemoveMatch'><arg type='s' direction='in'/></method>"
    "<method name='GetConnectionUnixUser'><arg type='s' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='GetConnectionCredentials'><arg type='s' direction='in'/><arg type='a{sv}' direction='out'/></method>"
    "<method name='GetConnectionUnixProcessID'><arg type='s' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='GetId'><arg type='s' direction='out'/></method>"
    "<signal name='NameOwnerChanged'><arg type='s'/><arg type='s'/><arg type='s'/></signal>"
    "<signal name='NameAcquired'><arg type='s'/></signal><signal name='NameLost'><arg type='s'/></signal>"
    "</interface><interface name='org.freedesktop.DBus.Peer'>"
    "<method name='Ping'/><method name='GetMachineId'><arg type='s' direction='out'/></method>"
    "</interface><interface name='org.freedesktop.DBus.Introspectable'>"
    "<method name='Introspect'><arg type='s' direction='out'/></method>"
    "</interface></node>";

static gboolean name_is_denied(const char *name) {
    return g_str_equal(name, "org.freedesktop.Flatpak") || g_str_has_prefix(name, "org.freedesktop.Flatpak.") ||
           g_str_equal(name, "org.freedesktop.systemd1") || g_str_has_prefix(name, "org.freedesktop.systemd1.") ||
           g_str_equal(name, "org.freedesktop.PackageKit") ||
           g_str_has_prefix(name, "org.freedesktop.PackageKit.");
}
static guint access_for(Broker *b, const char *name) {
    return GPOINTER_TO_UINT(g_hash_table_lookup(b->rules, name));
}
static Client *find_client(Broker *b, const char *unique) {
    for (guint i = 0; i < b->clients->len; i++) {
        Client *c = g_ptr_array_index(b->clients, i);
        if (g_strcmp0(c->unique, unique) == 0) return c;
    }
    return NULL;
}
static const char *owner_of(Broker *b, const char *name) {
    if (g_str_equal(name, "org.freedesktop.DBus")) return ":1.0";
    if (g_str_equal(name, ":1.0") && b->services->len != 0) return ":1.0";
    if (name[0] == ':') return find_client(b, name) ? name : NULL;
    Client *c = g_hash_table_lookup(b->owners, name);
    if (c != NULL) return c->unique;
    if (g_hash_table_contains(b->owners, name)) return ":1.0";
    return NULL;
}
static gboolean destination_has_talk_access(Broker *b, Client *destination) {
    GHashTableIter iter; gpointer key, value;
    g_hash_table_iter_init(&iter, b->owners);
    while (g_hash_table_iter_next(&iter, &key, &value))
        if (value == destination && (access_for(b, key) & ACCESS_TALK)) return TRUE;
    return FALSE;
}
static gboolean match_rule(Broker *broker, const char *rule, GDBusMessage *m) {
    GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
    const char *start = rule; gboolean quoted = FALSE, escaped = FALSE;
    for (const char *p = rule; ; p++) {
        if (*p == '\0' || (*p == ',' && !quoted && !escaped)) {
            g_ptr_array_add(parts, g_strndup(start, p - start));
            if (*p == '\0') break;
            start = p + 1;
        } else if (*p == '\\' && quoted && !escaped) escaped = TRUE;
        else { if (*p == '\'' && !escaped) quoted = !quoted; escaped = FALSE; }
    }
    g_ptr_array_add(parts, NULL);
    gboolean ok = TRUE;
    for (guint i = 0; i + 1 < parts->len && ok; i++) {
        char *part = g_strstrip(g_ptr_array_index(parts, i));
        char *eq = strchr(part, '=');
        if (eq == NULL) { ok = FALSE; break; }
        *eq++ = '\0';
        char *key = g_strstrip(part), *value = g_strstrip(eq);
        gsize n = strlen(value);
        if (n >= 2 && value[0] == '\'' && value[n - 1] == '\'') {
            value[n - 1] = '\0'; value++;
            char *read = value, *write = value;
            while (*read != '\0') {
                if (*read == '\\') {
                    read++;
                    if (*read != '\\' && *read != '\'') { ok = FALSE; break; }
                }
                *write++ = *read++;
            }
            *write = '\0';
            if (!ok) break;
        }
        const char *actual = NULL;
        if (g_str_equal(key, "type")) {
            GDBusMessageType t = g_dbus_message_get_message_type(m);
            actual = t == G_DBUS_MESSAGE_TYPE_SIGNAL ? "signal" : "";
        } else if (g_str_equal(key, "sender")) actual = g_dbus_message_get_sender(m);
        else if (g_str_equal(key, "interface")) actual = g_dbus_message_get_interface(m);
        else if (g_str_equal(key, "member")) actual = g_dbus_message_get_member(m);
        else if (g_str_equal(key, "path")) actual = g_dbus_message_get_path(m);
        else if (g_str_equal(key, "destination")) actual = g_dbus_message_get_destination(m);
        else if (g_str_has_prefix(key, "arg")) {
            char *end = NULL;
            gint64 index = g_ascii_strtoll(key + 3, &end, 10);
            gboolean namespace_match = end != NULL && g_str_equal(end, "namespace");
            if (end == key + 3 || index < 0 || index >= 64 ||
                (end != NULL && *end != '\0' && !namespace_match)) { ok = FALSE; break; }
            GVariant *body = g_dbus_message_get_body(m);
            if (body != NULL && (guint64)index < g_variant_n_children(body)) {
                GVariant *arg = g_variant_get_child_value(body, (gsize)index);
                if (g_variant_is_of_type(arg, G_VARIANT_TYPE_STRING) ||
                    g_variant_is_of_type(arg, G_VARIANT_TYPE_OBJECT_PATH) ||
                    g_variant_is_of_type(arg, G_VARIANT_TYPE_SIGNATURE))
                    actual = g_variant_get_string(arg, NULL);
                ok = actual != NULL && (namespace_match ?
                    (g_str_has_prefix(actual, value) &&
                     (actual[strlen(value)] == '\0' || actual[strlen(value)] == '.')) :
                    g_str_equal(actual, value));
                g_variant_unref(arg);
            } else ok = FALSE;
            continue;
        }
        else { ok = FALSE; break; }
        ok = actual != NULL && (g_str_equal(actual, value) ||
            (g_str_equal(key, "sender") && owner_of(broker, value) != NULL &&
             g_str_equal(actual, owner_of(broker, value))));
    }
    g_ptr_array_unref(parts);
    return ok;
}
static gboolean match_rule_supported(const char *rule) {
    char **parts = g_strsplit(rule, ",", -1);
    gboolean supported = TRUE;
    for (guint i = 0; parts[i] != NULL && supported; i++) {
        char *part = g_strstrip(parts[i]);
        char *eq = strchr(part, '=');
        if (eq == NULL) { supported = FALSE; break; }
        *eq = '\0';
        char *key = g_strstrip(part);
        if (g_str_equal(key, "type") || g_str_equal(key, "sender") ||
                g_str_equal(key, "interface") || g_str_equal(key, "member") ||
                g_str_equal(key, "path") || g_str_equal(key, "destination")) continue;
        if (!g_str_has_prefix(key, "arg")) { supported = FALSE; break; }
        char *end = NULL;
        guint64 index = g_ascii_strtoull(key + 3, &end, 10);
        if (end == key + 3 || index >= 64 ||
                (*end != '\0' && !g_str_equal(end, "namespace"))) supported = FALSE;
    }
    g_strfreev(parts);
    return supported;
}
static void send_raw(Client *c, GDBusMessage *m) {
    if (g_dbus_connection_is_closed(c->connection)) return;
    /* Serial numbers belong to the sending connection, not the source peer. */
    g_dbus_message_set_serial(m, 0);
    GError *error = NULL;
    g_dbus_connection_send_message(c->connection, m, G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                   NULL, &error);
    if (error != NULL) { g_warning("send to %s failed: %s", c->unique, error->message); g_error_free(error); }
}
static void emit_one(Client *c, const char *interface, const char *member,
                     GVariant *body) {
    GDBusMessage *signal = g_dbus_message_new_signal("/org/freedesktop/DBus", interface, member);
    g_dbus_message_set_sender(signal, "org.freedesktop.DBus");
    g_dbus_message_set_destination(signal, c->unique);
    g_dbus_message_set_body(signal, body);
    send_raw(c, signal);
    g_object_unref(signal);
}
static void emit_changed(Broker *b, const char *name, const char *old_owner,
                         const char *new_owner) {
    GVariant *body = g_variant_new("(sss)", name, old_owner ? old_owner : "",
                                   new_owner ? new_owner : "");
    GDBusMessage *signal = g_dbus_message_new_signal("/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameOwnerChanged");
    g_dbus_message_set_sender(signal, "org.freedesktop.DBus");
    g_dbus_message_set_body(signal, body);
    for (guint i = 0; i < b->clients->len; i++) {
        Client *c = g_ptr_array_index(b->clients, i);
        for (guint j = 0; j < c->matches->len; j++) {
            if (match_rule(b, g_ptr_array_index(c->matches, j), signal)) {
                emit_one(c, "org.freedesktop.DBus", "NameOwnerChanged", body);
                break;
            }
        }
    }
    g_object_unref(signal);
}
static void emit_name(Client *c, const char *member, const char *name) {
    emit_one(c, "org.freedesktop.DBus", member, g_variant_new("(s)", name));
}
static void return_dbus_error(GDBusMethodInvocation *inv, const char *name,
                              const char *message) {
    g_dbus_method_invocation_return_dbus_error(inv, name, message);
}

static const char *expected_signature(const char *interface, const char *method) {
    if (g_str_equal(interface, "org.freedesktop.DBus.Peer")) {
        if (g_str_equal(method, "Ping") || g_str_equal(method, "GetMachineId")) return "()";
    } else if (g_str_equal(interface, "org.freedesktop.DBus.Introspectable")) {
        if (g_str_equal(method, "Introspect")) return "()";
    } else if (g_str_equal(interface, "org.freedesktop.DBus")) {
        if (g_str_equal(method, "Hello") || g_str_equal(method, "ListNames") ||
                g_str_equal(method, "ListActivatableNames") || g_str_equal(method, "GetId")) return "()";
        if (g_str_equal(method, "RequestName") || g_str_equal(method, "StartServiceByName")) return "(su)";
        if (g_str_equal(method, "ReleaseName") || g_str_equal(method, "GetNameOwner") ||
                g_str_equal(method, "NameHasOwner") || g_str_equal(method, "AddMatch") ||
                g_str_equal(method, "RemoveMatch") || g_str_equal(method, "GetConnectionUnixUser") ||
                g_str_equal(method, "GetConnectionUnixProcessID") || g_str_equal(method,"GetConnectionCredentials")) return "(s)";
    }
    return NULL;
}

static void bus_method_call(GDBusConnection *connection, const char *sender,
                            const char *path, const char *interface,
                            const char *method, GVariant *parameters,
                            GDBusMethodInvocation *inv, gpointer user_data) {
    (void)connection; (void)path;
    Client *c = user_data; Broker *b = c->broker;
    const char *signature = expected_signature(interface, method);
    if (signature != NULL && !g_variant_is_of_type(parameters, G_VARIANT_TYPE(signature))) {
        return_dbus_error(inv, "org.freedesktop.DBus.Error.InvalidArgs", "Method body has the wrong signature");
        return;
    }
    if (g_str_equal(interface, "org.freedesktop.DBus.Peer")) {
        if (g_str_equal(method, "Ping")) g_dbus_method_invocation_return_value(inv, NULL);
        else if (g_str_equal(method, "GetMachineId")) {
            char *contents = NULL; gsize length = 0;
            char *id = NULL;
            if (g_file_get_contents("/etc/machine-id", &contents, &length, NULL)) {
                g_strstrip(contents);
                if (strlen(contents) == 32) id = g_strdup(contents);
            }
            g_free(contents);
            if (id == NULL) {
                char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, g_get_host_name(), -1);
                id = g_strndup(sum, 32); g_free(sum);
            }
            g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", id)); g_free(id);
        }
        return;
    }
    if (g_str_equal(interface, "org.freedesktop.DBus.Introspectable")) {
        if (g_str_equal(method, "Introspect"))
            g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", dbus_xml));
        return;
    }
    if (g_str_equal(method, "Hello")) {
        if (c->unique != NULL) {
            return_dbus_error(inv, "org.freedesktop.DBus.Error.AlreadyRegistered", "Connection already said Hello");
            return;
        }
        c->unique = g_strdup_printf(":1.%u", b->next_id++);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", c->unique));
    } else if (g_str_equal(method, "RequestName")) {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(su)"))) { return_dbus_error(inv, "org.freedesktop.DBus.Error.InvalidArgs", "Expected (su)"); return; }
        const char *name; guint flags; g_variant_get(parameters, "(&su)", &name, &flags);
        if (c->unique == NULL || !g_dbus_is_name(name) || name[0] == ':' ||
            g_str_equal(name, "org.freedesktop.DBus") || (flags & ~7u) != 0) {
            return_dbus_error(inv, "org.freedesktop.DBus.Error.InvalidArgs", "Invalid bus name"); return;
        }
        gboolean trusted_portal=FALSE;
        if (b->flatpak_portal && g_str_equal(name,"org.freedesktop.portal.Flatpak")) {
            if (b->host_session && b->flatpak_error[0]) {
                return_dbus_error(inv,MATON_SESSION_FLATPAK_ERROR,b->flatpak_error); return;
            }
            GCredentials* credentials=g_dbus_connection_get_peer_credentials(c->connection);
            trusted_portal=managed_portal_credentials(b,credentials,c->portal_generation);
            if (!trusted_portal) {
                return_dbus_error(inv,"org.freedesktop.DBus.Error.AccessDenied","Only the managed portal may own this name"); return;
            }
        }
        if (name_is_denied(name) || (!(access_for(b, name) & ACCESS_OWN) && !trusted_portal)) {
            return_dbus_error(inv, "org.freedesktop.DBus.Error.AccessDenied", "Name is denied by broker policy"); return;
        }
        const char *old = owner_of(b, name);
        Client *old_client = g_hash_table_lookup(b->owners, name);
        if (old != NULL && old_client != c) {
            guint old_flags = GPOINTER_TO_UINT(g_hash_table_lookup(b->name_flags, name));
            if (old_client != NULL && (flags & 2u) && (old_flags & 1u)) {
                emit_name(old_client, "NameLost", name);
                g_hash_table_replace(b->owners, g_strdup(name), c);
                g_hash_table_replace(b->name_flags, g_strdup(name), GUINT_TO_POINTER(flags));
                emit_name(c, "NameAcquired", name);
                emit_changed(b, name, old, c->unique);
                g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 1u));
            } else g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 3u));
        } else if (old_client == c) g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 4u));
        else {
            g_hash_table_insert(b->owners, g_strdup(name), c);
            g_hash_table_insert(b->name_flags, g_strdup(name), GUINT_TO_POINTER(flags));
            emit_name(c, "NameAcquired", name); emit_changed(b, name, NULL, c->unique);
            g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 1u));
        }
        if (trusted_portal && g_hash_table_lookup(b->owners,name)==c && b->ready_fd>=0) {
            char ready=1; (void)!send(b->ready_fd,&ready,1,MSG_NOSIGNAL);
            if (!b->host_session) close(b->ready_fd);
            b->ready_fd=-1;
        }
    } else if (g_str_equal(method, "ReleaseName")) {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(s)"))) { return_dbus_error(inv, "org.freedesktop.DBus.Error.InvalidArgs", "Expected (s)"); return; }
        const char *name; g_variant_get(parameters, "(&s)", &name);
        Client *owner = g_hash_table_lookup(b->owners, name);
        if (!g_hash_table_contains(b->owners, name)) g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 3u));
        else if (owner != c) g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 2u));
        else {
            char *old = g_strdup(c->unique); g_hash_table_remove(b->owners, name);
            g_hash_table_remove(b->name_flags, name);
            emit_name(c, "NameLost", name); emit_changed(b, name, old, NULL); g_free(old);
            g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 1u));
        }
    } else if (g_str_equal(method, "GetNameOwner") || g_str_equal(method, "NameHasOwner")) {
        const char *name; g_variant_get(parameters, "(&s)", &name); const char *owner = owner_of(b, name);
        if (g_str_equal(method, "NameHasOwner")) g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", owner != NULL));
        else if (owner == NULL) return_dbus_error(inv, "org.freedesktop.DBus.Error.NameHasNoOwner", "Name has no owner");
        else g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", owner));
    } else if (g_str_equal(method, "ListNames")) {
        GVariantBuilder a; g_variant_builder_init(&a, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&a, "s", "org.freedesktop.DBus");
        if (b->services->len != 0) g_variant_builder_add(&a, "s", ":1.0");
        for (guint i = 0; i < b->clients->len; i++) {
            Client *x = g_ptr_array_index(b->clients, i); if (x->unique) g_variant_builder_add(&a, "s", x->unique);
        }
        GHashTableIter it; gpointer key; g_hash_table_iter_init(&it, b->owners);
        while (g_hash_table_iter_next(&it, &key, NULL)) g_variant_builder_add(&a, "s", (char *)key);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(as)", &a));
    } else if (g_str_equal(method, "ListActivatableNames")) {
        GVariantBuilder a; g_variant_builder_init(&a, G_VARIANT_TYPE("as"));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(as)", &a));
    } else if (g_str_equal(method, "StartServiceByName")) {
        const char *name; guint flags; g_variant_get(parameters, "(&su)", &name, &flags);
        if (!g_dbus_is_name(name) || name[0] == ':' || flags != 0) {
            return_dbus_error(inv, "org.freedesktop.DBus.Error.InvalidArgs", "Invalid activation request"); return;
        }
        if ((name_is_denied(name) && !(g_hash_table_contains(b->owners,name) && g_hash_table_lookup(b->owners,name)==NULL)) || !(access_for(b, name) & ACCESS_TALK)) {
            /* Nothing to activate: a service MatonOS does not provide is
             * absent (apps treat ServiceUnknown as "not installed"; an
             * AccessDenied surfaced as a LibreOffice error dialog). An
             * existing but disallowed one is still denied. */
            if (owner_of(b, name) == NULL)
                return_dbus_error(inv, "org.freedesktop.DBus.Error.ServiceUnknown", "The name is not provided by any service");
            else
                return_dbus_error(inv, "org.freedesktop.DBus.Error.AccessDenied", "Activation is denied by broker policy");
            return;
        }
        if (owner_of(b, name) != NULL)
            g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 2u));
        else
            return_dbus_error(inv, "org.freedesktop.DBus.Error.ServiceUnknown", "No activatable service is configured");
    } else if (g_str_equal(method, "AddMatch") || g_str_equal(method, "RemoveMatch")) {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(s)"))) { return_dbus_error(inv, "org.freedesktop.DBus.Error.InvalidArgs", "Expected (s)"); return; }
        const char *rule; g_variant_get(parameters, "(&s)", &rule);
        if (strlen(rule) > 4096) { return_dbus_error(inv, "org.freedesktop.DBus.Error.MatchRuleInvalid", "Match rule too long"); return; }
        /* This broker implements only these keys. Reject rules it cannot honor. */
        if (!match_rule_supported(rule)) {
            return_dbus_error(inv, "org.freedesktop.DBus.Error.MatchRuleInvalid", "Unsupported match rule key"); return;
        }
        gboolean add = g_str_equal(method, "AddMatch");
        if (add) {
            if (c->matches->len >= MAX_MATCHES_PER_CLIENT) { return_dbus_error(inv, "org.freedesktop.DBus.Error.LimitsExceeded", "Too many match rules"); return; }
            g_ptr_array_add(c->matches, g_strdup(rule));
        }
        else {
            gboolean removed = FALSE;
            for (guint i = 0; i < c->matches->len; i++) if (g_str_equal(rule, g_ptr_array_index(c->matches, i))) {
                g_ptr_array_remove_index(c->matches, i); removed = TRUE; break;
            }
            if (!removed) { return_dbus_error(inv, "org.freedesktop.DBus.Error.MatchRuleNotFound", "No matching rule"); return; }
        }
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_str_equal(method,"GetConnectionCredentials")) {
        const char* name;g_variant_get(parameters,"(&s)",&name);
        const char* unique=owner_of(b,name);
        Client* target=unique ? find_client(b,unique) : NULL;
        GCredentials* credentials=target ? g_dbus_connection_get_peer_credentials(target->connection) : NULL;
        gboolean internal=g_strcmp0(unique,":1.0")==0;
        if (!credentials && !internal) {return_dbus_error(inv,"org.freedesktop.DBus.Error.NameHasNoOwner","Peer credentials are unavailable");return;}
        GError* ce=NULL;uid_t uid=internal ? b->owner_uid : g_credentials_get_unix_user(credentials,&ce);
        pid_t pid=internal ? getpid() : g_credentials_get_unix_pid(credentials,NULL);
        if (ce || pid<=0) {g_clear_error(&ce);return_dbus_error(inv,"org.freedesktop.DBus.Error.Failed","Peer credentials are unavailable");return;}
        GVariantBuilder result;g_variant_builder_init(&result,G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&result,"{sv}","UnixUserID",g_variant_new_uint32(uid));
        g_variant_builder_add(&result,"{sv}","ProcessID",g_variant_new_uint32(pid));
        g_dbus_method_invocation_return_value(inv,g_variant_new("(a{sv})",&result));
    } else if (g_str_equal(method, "GetConnectionUnixUser") || g_str_equal(method, "GetConnectionUnixProcessID")) {
        const char *name; g_variant_get(parameters, "(&s)", &name);
        if (g_str_equal(name, "org.freedesktop.DBus")) { guint value=g_str_equal(method,"GetConnectionUnixUser") ? (guint)b->owner_uid : (guint)getpid();g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", value)); return; }
        if (g_str_equal(name, ":1.0")) {
            guint value = g_str_equal(method, "GetConnectionUnixUser") ? (guint)b->owner_uid : (guint)getpid();
            g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", value)); return;
        }
        const char *unique = owner_of(b, name);
        if (g_strcmp0(unique, ":1.0") == 0) {
            guint value = g_str_equal(method, "GetConnectionUnixUser") ? (guint)b->owner_uid : (guint)getpid();
            g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", value)); return;
        }
        Client *target = unique ? find_client(b, unique) : NULL;
        if (target == NULL) { return_dbus_error(inv, "org.freedesktop.DBus.Error.NameHasNoOwner", "Connection does not exist"); return; }
        GCredentials *credentials = g_dbus_connection_get_peer_credentials(target->connection);
        guint value = (guint)b->owner_uid;
        if (g_str_equal(method, "GetConnectionUnixProcessID")) {
            if (credentials == NULL) { return_dbus_error(inv, "org.freedesktop.DBus.Error.Failed", "Peer process credentials are unavailable"); return; }
            pid_t pid = g_credentials_get_unix_pid(credentials, NULL);
            if (pid <= 0) { return_dbus_error(inv, "org.freedesktop.DBus.Error.Failed", "Peer process ID is unavailable"); return; }
            value = (guint)pid;
        } else if (credentials != NULL) {
            GError *ce = NULL; uid_t uid = g_credentials_get_unix_user(credentials, &ce);
            if (ce == NULL) value = (guint)uid; else g_clear_error(&ce);
        }
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", value));
    } else if (g_str_equal(method, "GetId")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", b->id));
    } else {
        (void)sender;
        return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown bus method");
    }
}

static const GDBusInterfaceVTable bus_vtable = { .method_call = bus_method_call };

static void discard_client(Broker *b, Client *c) {
    if (c->closed_id) g_signal_handler_disconnect(c->connection, c->closed_id);
    if (c->filter_id) g_dbus_connection_remove_filter(c->connection, c->filter_id);
    g_dbus_connection_close(c->connection, NULL, NULL, NULL);
    for (guint i = 0; i < b->clients->len; i++) if (g_ptr_array_index(b->clients, i) == c) {
        g_ptr_array_remove_index(b->clients, i); break;
    }
    g_hash_table_unref(c->pending); g_ptr_array_unref(c->outgoing); g_ptr_array_unref(c->matches);
    g_free(c->unique); g_object_unref(c->connection); g_free(c);
}

static gboolean is_local_service(Broker *b, const char *name) {
    return g_hash_table_contains(b->owners, name) && g_hash_table_lookup(b->owners, name) == NULL;
}
static gboolean internal_service_has_talk_access(Broker *b, GDBusMessage *message) {
    const char *path = g_dbus_message_get_path(message);
    const char *interface = g_dbus_message_get_interface(message);
    if(path && (g_str_has_prefix(path,"/org/freedesktop/portal/desktop/request/") ||
                g_str_has_prefix(path,"/org/freedesktop/portal/desktop/session/")) &&
            (access_for(b,"org.freedesktop.portal.Desktop") & ACCESS_TALK))return TRUE;
    for (guint i = 0; i < b->services->len; i++) {
        Service *service = g_ptr_array_index(b->services, i);
        if (g_strcmp0(path, service->path) == 0 &&
                (g_strcmp0(interface, service->interface_info->name) == 0 ||
                 g_strcmp0(interface,"org.freedesktop.DBus.Properties")==0 ||
                 g_strcmp0(interface,"org.freedesktop.DBus.Introspectable")==0) &&
                (access_for(b, service->name) & ACCESS_TALK)) return TRUE;
    }
    return FALSE;
}
static gboolean denied_call(GDBusMessage *m) {
    const char *dest = g_dbus_message_get_destination(m), *iface = g_dbus_message_get_interface(m), *member = g_dbus_message_get_member(m);
    if (dest == NULL) return FALSE;
    if (g_str_equal(dest,"org.freedesktop.Flatpak") &&
            g_strcmp0(g_dbus_message_get_path(m),"/org/freedesktop/Flatpak/SessionHelper")==0 &&
            (g_strcmp0(iface,"org.freedesktop.Flatpak.SessionHelper")==0 ||
             g_strcmp0(iface,"org.freedesktop.DBus.Properties")==0 ||
             g_strcmp0(iface,"org.freedesktop.DBus.Introspectable")==0)) return FALSE;
    if (name_is_denied(dest)) return TRUE;
    if ((g_str_equal(dest, "org.freedesktop.PackageKit") || g_str_has_prefix(dest, "org.freedesktop.PackageKit.")) &&
        member != NULL && (g_strrstr(member, "Install") != NULL || g_strrstr(member, "Remove") != NULL)) return TRUE;
    if (iface != NULL && g_str_equal(iface, "org.freedesktop.Flatpak")) return TRUE;
    return FALSE;
}
static GDBusMessage *filter_message(GDBusConnection *connection, GDBusMessage *message,
                                    gboolean incoming, gpointer data) {
    (void)connection;
    Client *c = data; Broker *b = c->broker;
    if (!incoming) return message;
    GDBusMessageType type = g_dbus_message_get_message_type(message);
    if (type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN || type == G_DBUS_MESSAGE_TYPE_ERROR) {
        guint32 reply = g_dbus_message_get_reply_serial(message);
        Pending *p = g_hash_table_lookup(c->pending, GUINT_TO_POINTER(reply));
        if (p != NULL) {
            GDBusMessage *forward = g_dbus_message_copy(message, NULL);
            g_dbus_message_set_reply_serial(forward, p->serial);
            g_dbus_message_set_destination(forward, p->origin->unique);
            g_dbus_message_set_sender(forward, c->unique);
            send_raw(p->origin, forward); g_object_unref(forward);
            g_ptr_array_remove(p->origin->outgoing, p);
            g_hash_table_remove(c->pending, GUINT_TO_POINTER(reply));
        }
        return NULL;
    }
    if (type == G_DBUS_MESSAGE_TYPE_SIGNAL) {
        if (c->unique == NULL) return NULL;
        GDBusMessage *routed = g_dbus_message_copy(message, NULL);
        g_dbus_message_set_sender(routed, c->unique);
        const char *sender = c->unique;
        const char* destination=g_dbus_message_get_destination(routed);
        if (destination) {
            const char* unique=owner_of(b,destination);
            Client* target=unique ? find_client(b,unique) : NULL;
            // Directed signals require no AddMatch and must never be broadcast.
            gboolean portal_sender = c->portal_generation==g_atomic_int_get(&b->portal_generation) &&
                b->flatpak_portal && b->portal_pid > 0 &&
                g_hash_table_lookup(b->owners, "org.freedesktop.portal.Flatpak") == c;
            gboolean allowed = destination[0] == ':' ?
                destination_has_talk_access(b, target) : ((access_for(b, destination) & ACCESS_TALK)!=0);
            if (target && (portal_sender || allowed)) send_raw(target,routed);
            g_object_unref(routed);return NULL;
        }
        for (guint j = 0; j < b->clients->len; j++) {
            Client *dst = g_ptr_array_index(b->clients, j);
            if (dst == c && dst->matches->len == 0) continue;
            for (guint k = 0; k < dst->matches->len; k++)
                if (match_rule(b, g_ptr_array_index(dst->matches, k), routed)) {
                    GDBusMessage *copy = g_dbus_message_copy(routed, NULL);
                    g_dbus_message_set_sender(copy, sender);
                    send_raw(dst, copy); g_object_unref(copy); break;
                }
        }
        g_object_unref(routed);
        return NULL;
    }
    if (type != G_DBUS_MESSAGE_TYPE_METHOD_CALL) return message;
    const char *dest = g_dbus_message_get_destination(message);
    if (b->trace_calls) {
        GVariant *body = g_dbus_message_get_body(message);
        char *args = body ? g_variant_print(body, TRUE) : g_strdup("()");
        g_printerr("CALL serial=%u sender=%s destination=%s path=%s interface=%s member=%s args=%s\n",
            g_dbus_message_get_serial(message), c->unique ? c->unique : "<pre-Hello>",
            dest ? dest : "<none>", g_dbus_message_get_path(message) ? g_dbus_message_get_path(message) : "<none>",
            g_dbus_message_get_interface(message) ? g_dbus_message_get_interface(message) : "<none>",
            g_dbus_message_get_member(message) ? g_dbus_message_get_member(message) : "<none>", args);
        g_free(args);
    }
    gboolean control_path = (g_strcmp0(g_dbus_message_get_path(message), "/org/freedesktop/DBus") == 0 ||
        g_strcmp0(g_dbus_message_get_path(message), "/") == 0) &&
        g_strcmp0(dest, "org.freedesktop.DBus") == 0;
    if (c->unique == NULL && !(control_path &&
            g_strcmp0(g_dbus_message_get_interface(message), "org.freedesktop.DBus") == 0 &&
            g_strcmp0(g_dbus_message_get_member(message), "Hello") == 0)) {
        GDBusMessage *err = g_dbus_message_new_method_error_literal(message,
            "org.freedesktop.DBus.Error.AccessDenied", "Hello must be called before other bus methods");
        send_raw(c, err); g_object_unref(err); return NULL;
    }
    if (g_strcmp0(g_dbus_message_get_path(message), "/org/freedesktop/DBus") == 0 && !control_path) {
        GDBusMessage *err = g_dbus_message_new_method_error_literal(message,
            "org.freedesktop.DBus.Error.AccessDenied", "Control path forwarding is denied");
        send_raw(c, err); g_object_unref(err); return NULL;
    }
    if (control_path &&
        (g_strcmp0(g_dbus_message_get_interface(message), "org.freedesktop.DBus") == 0 ||
         g_strcmp0(g_dbus_message_get_interface(message), "org.freedesktop.DBus.Peer") == 0 ||
         g_strcmp0(g_dbus_message_get_interface(message), "org.freedesktop.DBus.Introspectable") == 0))
        return message;
    /* A service MatonOS does not provide is absent, not forbidden: with no
     * owner the call fails with ServiceUnknown, as on a system without it
     * (systemd, logind, PackageKit). Existing but disallowed services still
     * answer AccessDenied. */
    if (denied_call(message) && dest != NULL && owner_of(b, dest) == NULL) {
        GDBusMessage *err = g_dbus_message_new_method_error(message,
            "org.freedesktop.DBus.Error.ServiceUnknown", "The name %s was not provided by any .service files", dest);
        send_raw(c, err); g_object_unref(err); return NULL;
    }
    if (denied_call(message)) {
        GDBusMessage *err = g_dbus_message_new_method_error_literal(message,
            "org.freedesktop.DBus.Error.AccessDenied", "Destination is denied by broker policy");
        send_raw(c, err); g_object_unref(err); return NULL;
    }
    if (dest == NULL) {
        GDBusMessage *err = g_dbus_message_new_method_error_literal(message,
            "org.freedesktop.DBus.Error.AccessDenied", "Peer method calls require a destination");
        send_raw(c, err); g_object_unref(err); return NULL;
    }
    if (is_local_service(b, dest)) {
        if (!(access_for(b, dest) & ACCESS_TALK)) {
            GDBusMessage *err = g_dbus_message_new_method_error_literal(message,
                "org.freedesktop.DBus.Error.AccessDenied", "Destination is denied by broker policy");
            send_raw(c, err); g_object_unref(err); return NULL;
        }
        return message;
    }
    if (g_str_equal(dest, ":1.0") && b->services->len != 0) {
        if (!internal_service_has_talk_access(b, message)) {
            GDBusMessage *err = g_dbus_message_new_method_error_literal(message,
                "org.freedesktop.DBus.Error.AccessDenied", "Destination is denied by broker policy");
            send_raw(c, err); g_object_unref(err); return NULL;
        }
        return message;
    }
    const char *target_name = dest;
    Client *target = dest[0] == ':' ? find_client(b, dest) : g_hash_table_lookup(b->owners, dest);
    if (target == NULL) {
        GDBusMessage *err = g_dbus_message_new_method_error_literal(message,
            "org.freedesktop.DBus.Error.ServiceUnknown", "The name has no owner");
        send_raw(c, err); g_object_unref(err); return NULL;
    }
    if ((dest[0] != ':' && !(access_for(b, target_name) & ACCESS_TALK)) ||
        (dest[0] == ':' && !destination_has_talk_access(b, target))) {
        GDBusMessage *err = g_dbus_message_new_method_error_literal(message,
            "org.freedesktop.DBus.Error.AccessDenied", "Destination is denied by broker policy");
        send_raw(c, err); g_object_unref(err); return NULL;
    }
    if (c->outgoing->len >= MAX_PENDING_PER_CLIENT || g_hash_table_size(target->pending) >= MAX_PENDING_PER_CLIENT) {
        GDBusMessage *err = g_dbus_message_new_method_error_literal(message,
            "org.freedesktop.DBus.Error.LimitsExceeded", "Too many pending method calls");
        send_raw(c, err); g_object_unref(err); return NULL;
    }
    GDBusMessage *copy = g_dbus_message_copy(message, NULL);
    g_dbus_message_set_sender(copy, c->unique);
    g_dbus_message_set_serial(copy, 0);
    guint32 forwarded = 0;
    GError *error = NULL;
    if (g_dbus_connection_send_message(target->connection, copy,
            G_DBUS_SEND_MESSAGE_FLAGS_NONE, &forwarded, &error)) {
        Pending *p = g_new0(Pending, 1); p->origin = c; p->serial = g_dbus_message_get_serial(message);
        g_ptr_array_add(c->outgoing, p);
        g_hash_table_insert(target->pending, GUINT_TO_POINTER(forwarded), p);
    } else {
        GDBusMessage *err = g_dbus_message_new_method_error_literal(message,
            "org.freedesktop.DBus.Error.ServiceUnknown", error ? error->message : "Destination unavailable");
        send_raw(c, err); g_object_unref(err); g_clear_error(&error);
    }
    g_object_unref(copy);
    return NULL;
}

static void client_closed(GDBusConnection *connection, gboolean vanished,
                          GError *error, gpointer data) {
    (void)connection; (void)vanished; (void)error;
    Client *c = data; Broker *b = c->broker;
    broker_session_client_closed(b,connection);
    /* Fail calls waiting on the connection that just disappeared. */
    GHashTableIter pending_iter; gpointer pending_key, pending_value;
    g_hash_table_iter_init(&pending_iter, c->pending);
    while (g_hash_table_iter_next(&pending_iter, &pending_key, &pending_value)) {
        Pending *p = pending_value;
        g_ptr_array_remove(p->origin->outgoing, p);
        GDBusMessage *request = g_dbus_message_new();
        g_dbus_message_set_message_type(request, G_DBUS_MESSAGE_TYPE_METHOD_CALL);
        g_dbus_message_set_serial(request, p->serial);
        GDBusMessage *failure = g_dbus_message_new_method_error_literal(request,
            "org.freedesktop.DBus.Error.ServiceUnknown", "Destination connection closed");
        send_raw(p->origin, failure); g_object_unref(failure); g_object_unref(request);
    }
    (void)pending_key;
    /* A disconnected origin must be removed from every destination table. */
    for (guint i = 0; i < c->outgoing->len; i++) {
        Pending *origin_pending = g_ptr_array_index(c->outgoing, i);
        for (guint j = 0; j < b->clients->len; j++) {
            Client *destination = g_ptr_array_index(b->clients, j);
            GHashTableIter iter; gpointer key, value;
            g_hash_table_iter_init(&iter, destination->pending);
            while (g_hash_table_iter_next(&iter, &key, &value))
                if (value == origin_pending) g_hash_table_iter_remove(&iter);
        }
    }
    g_ptr_array_set_size(c->outgoing, 0);
    for (guint i = 0; i < b->clients->len; i++) if (g_ptr_array_index(b->clients, i) == c) {
        GHashTableIter iter; gpointer key, value; GPtrArray *gone = g_ptr_array_new_with_free_func(g_free);
        g_hash_table_iter_init(&iter, b->owners);
        while (g_hash_table_iter_next(&iter, &key, &value)) if (value == c) g_ptr_array_add(gone, g_strdup(key));
        for (guint j = 0; j < gone->len; j++) {
            const char *name = g_ptr_array_index(gone, j); g_hash_table_remove(b->owners, name);
            g_hash_table_remove(b->name_flags, name);
            emit_name(c, "NameLost", name); emit_changed(b, name, c->unique, NULL);
        }
        g_ptr_array_unref(gone);
        g_ptr_array_remove_index(b->clients, i);
        if (c->filter_id) g_dbus_connection_remove_filter(c->connection, c->filter_id);
        if (c->closed_id) g_signal_handler_disconnect(c->connection, c->closed_id);
        g_hash_table_unref(c->pending); g_ptr_array_unref(c->outgoing); g_ptr_array_unref(c->matches);
        g_free(c->unique); g_object_unref(c->connection); g_free(c); break;
    }
}
static gboolean on_new_connection(GDBusServer *server, GDBusConnection *connection,
                                  gpointer data) {
    (void)server;
    Broker *b = data;
    if (b->clients->len >= MAX_CLIENTS) return FALSE;
    Client *c = g_new0(Client, 1); c->broker = b; c->connection = g_object_ref(connection);
    GIOStream *stream = g_dbus_connection_get_stream(connection);
    if (!G_IS_SOCKET_CONNECTION(stream)) { g_object_unref(c->connection); g_free(c); return FALSE; }
    int fd = g_socket_get_fd(g_socket_connection_get_socket(G_SOCKET_CONNECTION(stream)));
    struct ucred peercred; socklen_t peercred_len = sizeof(peercred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peercred, &peercred_len) != 0 ||
        peercred.uid != (b->host_session ? 1000u : b->owner_uid)) {
        g_object_unref(c->connection); g_free(c); return FALSE;
    }
    GCredentials *peer = g_dbus_connection_get_peer_credentials(connection);
    if (peer == NULL) { g_object_unref(c->connection); g_free(c); return FALSE; }
    GError *cred_error = NULL;
    uid_t peer_uid = g_credentials_get_unix_user(peer, &cred_error);
    if (cred_error != NULL || peer_uid != (b->host_session ? 1000u : b->owner_uid)) {
        g_clear_error(&cred_error); g_object_unref(c->connection); g_free(c); return FALSE;
    }
    c->portal_generation=g_atomic_int_get(&b->portal_generation);
    c->matches = g_ptr_array_new_with_free_func(g_free);
    c->outgoing = g_ptr_array_new();
    c->pending = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    c->closed_id = g_signal_connect(connection, "closed", G_CALLBACK(client_closed), c);
    g_ptr_array_add(b->clients, c);
    GError *error = NULL;
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(dbus_xml, &error);
    if (node == NULL) { g_warning("cannot parse bus interface: %s", error->message); g_error_free(error); discard_client(b, c); return FALSE; }
    const char *control_paths[] = { "/org/freedesktop/DBus", "/" };
    for (guint path = 0; path < G_N_ELEMENTS(control_paths); path++)
    for (guint i = 0; i < 3; i++) if (g_dbus_connection_register_object(connection, control_paths[path],
        node->interfaces[i], &bus_vtable, c, NULL, &error) == 0) {
        g_warning("cannot register bus interface: %s", error->message); g_clear_error(&error);
        g_dbus_node_info_unref(node); discard_client(b, c); return FALSE;
    }
    g_dbus_node_info_unref(node);
    for (guint i = 0; i < b->services->len; i++) {
        Service *s = g_ptr_array_index(b->services, i);
        if (g_dbus_connection_register_object(connection, s->path, s->interface_info,
            s->vtable, s->user_data, NULL, &error) == 0) {
            g_warning("cannot register service object: %s", error->message); g_clear_error(&error);
            discard_client(b, c); return FALSE;
        }
    }
    c->filter_id = g_dbus_connection_add_filter(connection, filter_message, c, NULL);
    return TRUE;
}

Broker *broker_new(const char *socket_path, const char *config_path, GError **error) {
    Broker *b = g_new0(Broker, 1); b->socket_path = g_strdup(socket_path); b->owner_uid = geteuid(); b->ready_fd=-1; b->control_listener=-1; b->control_fd=-1; b->portal_pidfd=-1;
    b->clients = g_ptr_array_new(); b->services = g_ptr_array_new();
    b->owners = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    b->name_flags = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    b->rules = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL); b->next_id = 1;
    b->trace_calls = g_strcmp0(g_getenv("MATONOS_DBUS_TRACE"), "1") == 0;
    b->id = g_dbus_generate_guid();
    gchar *contents = NULL; gsize length = 0;
    if (!g_file_get_contents(config_path, &contents, &length, error)) { broker_free(b); return NULL; }
    char **lines = g_strsplit(contents, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        char *line = g_strstrip(lines[i]); if (!*line || *line == '#') continue;
        char **p = g_strsplit_set(line, " \t", 2);
        if (p[0] == NULL || p[1] == NULL || !g_dbus_is_name(g_strstrip(p[1]))) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "invalid policy line %u", i + 1);
            g_strfreev(p); g_strfreev(lines); g_free(contents); broker_free(b); return NULL;
        }
        char *name = g_strstrip(p[1]); guint flags = access_for(b, name);
        if (g_str_equal(p[0], "own")) flags |= ACCESS_OWN;
        else if (g_str_equal(p[0], "talk")) flags |= ACCESS_TALK;
        else { g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "expected own or talk at policy line %u", i + 1); g_strfreev(p); g_strfreev(lines); g_free(contents); broker_free(b); return NULL; }
        g_hash_table_replace(b->rules, g_strdup(name), GUINT_TO_POINTER(flags)); g_strfreev(p);
    }
    g_strfreev(lines); g_free(contents);
    return b;
}

gboolean broker_add_service(Broker *b, const char *name, const char *path,
                            GDBusInterfaceInfo *interface, const GDBusInterfaceVTable *vtable,
                            gpointer user_data, GError **error) {
    if (!g_dbus_is_name(name) || !g_variant_is_object_path(path) || interface == NULL || interface->name == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid service name, path, or interface"); return FALSE;
    }
    if (g_hash_table_contains(b->owners, name) && g_hash_table_lookup(b->owners,name)!=NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "duplicate service name"); return FALSE;
    }
    /* One well-known name may export several interfaces, but not duplicate
     * registrations of the same interface at a path. */
    for(guint i=0;i<b->services->len;i++) {
        Service* old=g_ptr_array_index(b->services,i);
        if(!strcmp(old->path,path) && !strcmp(old->interface_info->name,interface->name)) {
            g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_EXISTS,"duplicate service interface");return FALSE;
        }
    }
    Service *s = g_new0(Service, 1); s->name = g_strdup(name); s->path = g_strdup(path);
    s->interface_info = g_dbus_interface_info_ref(interface); s->vtable = vtable; s->user_data = user_data;
    g_ptr_array_add(b->services, s); g_hash_table_insert(b->owners, g_strdup(name), NULL);
    return TRUE;
}
/* This endpoint is reachable only through the host's private directory
 * capability. The wrapper supervisor registers its gated, unreaped child. */
static gboolean control_closed(gint fd, GIOCondition condition, gpointer data) {
    (void)condition;
    Broker* b=data; char byte;
    if (recv(fd,&byte,1,MSG_DONTWAIT)<0 && errno==EAGAIN) return G_SOURCE_CONTINUE;
    /* Fail closed: disconnect all peers before a registered PID can be reused. */
    g_atomic_int_inc(&b->portal_generation);
    b->portal_pid=0; b->ready_fd=-1; b->monitor[0]=0;
    if(b->portal_pidfd>=0)close(b->portal_pidfd);
    b->portal_pidfd=-1;
    for (guint i=0;i<b->clients->len;i++) {
        Client* c=g_ptr_array_index(b->clients,i);
        g_dbus_connection_close(c->connection,NULL,NULL,NULL);
    }
    close(b->control_fd);b->control_fd=-1;b->control_client_source=0;
    return G_SOURCE_REMOVE;
}
static gboolean control_accept(gint fd, GIOCondition condition, gpointer data) {
    (void)condition; Broker* b=data;
    int client=accept4(fd,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
    if(client<0)return G_SOURCE_CONTINUE;
    struct ucred cred; socklen_t size=sizeof(cred);
    struct MatonSessionRegistration registration={0};
    /* SOCK_SEQPACKET preserves packet boundaries and bounds the wait. */
    struct pollfd ready={.fd=client,.events=POLLIN};
    if(getsockopt(client,SOL_SOCKET,SO_PEERCRED,&cred,&size) || cred.uid!=1000 ||
       poll(&ready,1,1000)<=0) {close(client);return G_SOURCE_CONTINUE;}
    ssize_t received=recv(client,&registration,sizeof(registration),MSG_TRUNC);
    if(received!=sizeof(registration) ||
       !memchr(registration.flatpak_version,0,sizeof(registration.flatpak_version)) ||
       !maton_flatpak_supported(registration.flatpak_version)) {
        struct MatonSessionReply response={.status=MATON_SESSION_FLATPAK_UNSUPPORTED};
        const char* version=received==sizeof(registration) &&
            memchr(registration.flatpak_version,0,sizeof(registration.flatpak_version)) ?
            registration.flatpak_version : "unknown (missing/invalid registration)";
        g_strlcpy(response.error,MATON_SESSION_FLATPAK_ERROR,sizeof(response.error));
        g_snprintf(response.message,sizeof(response.message),
            "System Flatpak %s is unsupported; broker requires >= %s (built against %s). Update the system Flatpak/image.",
            version,MATON_BROKER_MIN_FLATPAK_VERSION,MATON_BROKER_BUILT_FLATPAK_VERSION);
        g_warning("%s: %s; refusing portal registration",response.error,response.message);
        if(b->control_fd<0)g_strlcpy(b->flatpak_error,response.message,sizeof(b->flatpak_error));
        (void)!send(client,&response,sizeof(response),MSG_NOSIGNAL);
        close(client);return G_SOURCE_CONTINUE;
    }
    if(registration.pid<=0 || !memchr(registration.monitor,0,sizeof(registration.monitor)) ||
       !g_str_has_prefix(registration.monitor,"/data/matonos/linux/runtime/wayland-")) {close(client);return G_SOURCE_CONTINUE;}
    struct pollfd alive={.fd=b->portal_pidfd,.events=POLLIN};
    gboolean reusable=b->ready_fd<0 && b->portal_pidfd>=0 && poll(&alive,1,0)==0 &&
        g_hash_table_lookup(b->owners,"org.freedesktop.portal.Flatpak")!=NULL;
    struct MatonSessionReply response={
        .status=b->control_fd>=0 ? (reusable ? 2 : 3) : 0,
        .supervisor=b->control_fd>=0 ? b->supervisor_pid : cred.pid
    };
    if(response.status==0) {
        b->portal_pidfd=(int)syscall(SYS_pidfd_open,registration.pid,0);
        if(b->portal_pidfd<0) {g_warning("Cannot pin managed portal PID: %s",g_strerror(errno));response.status=3;(void)!send(client,&response,sizeof(response),MSG_NOSIGNAL);close(client);return G_SOURCE_CONTINUE;}
        b->flatpak_error[0]=0;
        g_atomic_int_inc(&b->portal_generation);
        b->supervisor_pid=cred.pid; b->portal_pid=registration.pid; b->control_fd=client;b->ready_fd=client;
        g_strlcpy(b->monitor,registration.monitor,sizeof(b->monitor));
        b->control_client_source=g_unix_fd_add(client,G_IO_IN|G_IO_HUP|G_IO_ERR,control_closed,b);
    }
    (void)!send(client,&response,sizeof(response),MSG_NOSIGNAL);
    if(response.status!=0)close(client);
    return G_SOURCE_CONTINUE;
}
gboolean broker_enable_host_session(Broker* b,GError** error) {
    b->host_session=TRUE;b->flatpak_portal=TRUE;
    g_message("APK session broker built against Flatpak %s; minimum supported system Flatpak %s",
        MATON_BROKER_BUILT_FLATPAK_VERSION,MATON_BROKER_MIN_FLATPAK_VERSION);
    b->control_path=g_strconcat(b->socket_path,"-control",NULL);
    struct sockaddr_un address={.sun_family=AF_UNIX};
    if(strlen(b->control_path)>=sizeof(address.sun_path))goto fail;
    strcpy(address.sun_path,b->control_path);
    /* The host removes stale sockets before launch, never replace arbitrary files. */
    b->control_listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(b->control_listener<0 || bind(b->control_listener,(struct sockaddr*)&address,sizeof(address)) ||
       chmod(b->control_path,0666) || listen(b->control_listener,8))goto fail;
    b->control_source=g_unix_fd_add(b->control_listener,G_IO_IN,control_accept,b);
    return TRUE;
fail:
    g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,"cannot create session portal control socket");return FALSE;
}
const char* broker_monitor_path(Broker* b) {return b->monitor;}
void broker_set_monitor_path(Broker* b,const char* path) {if(path)g_strlcpy(b->monitor,path,sizeof(b->monitor));}
gboolean broker_run(Broker *b, GError **error) {
    struct stat st;
    if (lstat(b->socket_path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != b->owner_uid) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "refusing to replace non-owned socket path"); return FALSE;
        }
        if (unlink(b->socket_path) != 0) { g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "unlink socket: %s", g_strerror(errno)); return FALSE; }
    }
    char *address = g_strdup_printf("unix:path=%s", b->socket_path);
    char *guid = g_dbus_generate_guid();
    b->server = g_dbus_server_new_sync(address, b->host_session ? G_DBUS_SERVER_FLAGS_NONE : G_DBUS_SERVER_FLAGS_AUTHENTICATION_REQUIRE_SAME_USER,
                                        guid, NULL, NULL, error);
    g_free(address); g_free(guid);
    if (b->server == NULL) return FALSE;
    if (chmod(b->socket_path, b->host_session ? 0666 : 0600) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "chmod broker socket: %s", g_strerror(errno));
        g_dbus_server_stop(b->server); g_clear_object(&b->server); return FALSE;
    }
    g_signal_connect(b->server, "new-connection", G_CALLBACK(on_new_connection), b);
    b->loop = g_main_loop_new(NULL, FALSE);
    if (b->loop == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot create broker main loop");
        g_dbus_server_stop(b->server); g_clear_object(&b->server); return FALSE;
    }
    g_dbus_server_start(b->server);
    g_print("DBUS_SESSION_BUS_ADDRESS=unix:path=%s\n", b->socket_path);
    fflush(stdout);
    g_main_loop_run(b->loop);
    return TRUE;
}
void broker_stop(Broker* b) {if(b->loop)g_main_loop_quit(b->loop);}
void broker_free(Broker *b) {
    if (!b) return;
    broker_session_services_free(b);
    if (b->portal_pidfd>=0)close(b->portal_pidfd);
    if (b->control_source) g_source_remove(b->control_source);
    if (b->control_client_source) g_source_remove(b->control_client_source);
    if (b->control_fd>=0) close(b->control_fd);
    if (b->control_listener>=0) close(b->control_listener);
    if (b->control_path) {unlink(b->control_path);g_free(b->control_path);}
    if (b->ready_fd>=0 && !b->host_session) close(b->ready_fd);
    if (b->server) { g_dbus_server_stop(b->server); g_object_unref(b->server); }
    if (b->loop) g_main_loop_unref(b->loop);
    if (b->clients) {
        while (b->clients->len) {
            Client *c = g_ptr_array_index(b->clients, b->clients->len - 1);
            if (c->closed_id) g_signal_handler_disconnect(c->connection, c->closed_id);
            if (c->filter_id) g_dbus_connection_remove_filter(c->connection, c->filter_id);
            g_dbus_connection_close_sync(c->connection, NULL, NULL);
            g_ptr_array_remove_index(b->clients, b->clients->len - 1);
            g_hash_table_unref(c->pending); g_ptr_array_unref(c->outgoing); g_ptr_array_unref(c->matches);
            g_free(c->unique); g_object_unref(c->connection); g_free(c);
        }
        g_ptr_array_unref(b->clients);
    }
    if (b->services) { for (guint i=0; i<b->services->len; i++) { Service *s=g_ptr_array_index(b->services,i); g_free(s->name); g_free(s->path); g_dbus_interface_info_unref(s->interface_info); g_free(s); } g_ptr_array_unref(b->services); }
    if (b->owners) g_hash_table_unref(b->owners);
    if (b->name_flags) g_hash_table_unref(b->name_flags);
    if (b->rules) g_hash_table_unref(b->rules);
    if (b->socket_path) { struct stat st; if (lstat(b->socket_path,&st)==0 && S_ISSOCK(st.st_mode) && st.st_uid==b->owner_uid) unlink(b->socket_path); }
    g_free(b->id); g_free(b->socket_path); g_free(b);
}

const char* broker_connection_name(Broker* b,GDBusConnection* connection) {
    for(guint i=0;i<b->clients->len;i++) {
        Client* c=g_ptr_array_index(b->clients,i);if(c->connection==connection)return c->unique;
    }
    return NULL;
}
void* broker_session_data(Broker* b){return b->session_services;}
void broker_set_session_data(Broker* b,void* data){b->session_services=data;}
