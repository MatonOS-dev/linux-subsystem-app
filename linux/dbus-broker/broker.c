#include "broker.h"

#include <errno.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <fcntl.h>
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
    int bridge_fd;
    guint bridge_source;
    gboolean bridge;
};
struct _Service {
    char *name, *path;
    GDBusInterfaceInfo *interface_info;
    const GDBusInterfaceVTable *vtable;
    gpointer user_data;
};
struct _Pending { Client *origin; guint32 serial; gboolean flatpak; };
struct _Broker {
    char *socket_path;
    uid_t owner_uid;
    GDBusServer *server;
    GMainLoop *loop;
    GPtrArray *clients;
    GPtrArray *services;
    GHashTable *owners; /* bus name -> Client* (NULL value for internal service) */
    GHashTable *name_flags; /* owned name -> RequestName flags */
    GHashTable *name_queues; /* well-known name -> GQueue of waiting Client* */
    GHashTable *rules;  /* name -> access flags */
    guint next_id;
    char *id;
    gboolean trace_calls;
    void* session_services;
};

enum { ACCESS_OWN = 1, ACCESS_TALK = 2 };
enum { MAX_CLIENTS = 1024, MAX_MATCHES_PER_CLIENT = 256, MAX_PENDING_PER_CLIENT = 256 };
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
/* Names an app may never claim through a wildcard rule: the portal and bus
 * services it talks to, plus the always-denied host services. The managed
 * portal claims org.freedesktop.portal.Flatpak through its own check. */
static gboolean name_is_reserved(const char *name) {
    return name_is_denied(name) ||
           g_str_equal(name, "org.freedesktop.secrets") ||
           g_str_has_prefix(name, "org.gnome.keyring") ||
           g_str_has_prefix(name, "org.kde.kwalletd") ||
           g_str_has_prefix(name, "org.freedesktop.portal.") ||
           g_str_has_prefix(name, "org.freedesktop.Flatpak") ||
           g_str_has_prefix(name, "org.freedesktop.impl.portal.") ||
           g_str_equal(name, "org.freedesktop.DBus") || g_str_has_prefix(name, "org.freedesktop.DBus.");
}
/* "own *" / "talk *": an app's private bus lets it register any other name
 * (its own services, e.g. Steam's launcher service) and call them. */
static guint access_for(Broker *b, const char *name) {
    guint access = GPOINTER_TO_UINT(g_hash_table_lookup(b->rules, name));
    if (!name_is_reserved(name)) access |= GPOINTER_TO_UINT(g_hash_table_lookup(b->rules, "*"));
    return access;
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
static gboolean bridge_send_message(Client *c, GDBusMessage *m) {
    GError *error = NULL;
    gsize size = 0;
    guchar *blob = g_dbus_message_to_blob(m, &size, G_DBUS_CAPABILITY_FLAGS_UNIX_FD_PASSING, &error);
    if (!blob) {
        g_warning("bridge %s serialize failed: %s", c->unique ? c->unique : "<unregistered>", error->message);
        g_clear_error(&error);
        return FALSE;
    }
    GUnixFDList *fd_list = g_dbus_message_get_unix_fd_list(m);
    int fd_count = fd_list ? g_unix_fd_list_get_length(fd_list) : 0;
    guint declared_fds = g_dbus_message_get_num_unix_fds(m);
    if (declared_fds != (guint)fd_count) {
        g_warning("bridge %s outgoing UNIX_FDS mismatch declared=%u fd_list=%d",
                  c->unique ? c->unique : "<unregistered>", declared_fds, fd_count);
        g_free(blob); return FALSE;
    }
    struct iovec iov = { .iov_base = blob, .iov_len = size };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    void *control = NULL;
    if (fd_count > 0) {
        gsize control_size = CMSG_SPACE(sizeof(int) * (gsize)fd_count);
        control = g_try_malloc0(control_size);
        if (!control) {
            g_warning("bridge %s cannot allocate SCM_RIGHTS send buffer fd_count=%d bytes=%zu",
                      c->unique ? c->unique : "<unregistered>", fd_count, control_size);
            g_free(blob); return FALSE;
        }
        msg.msg_control = control;
        msg.msg_controllen = control_size;
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * (gsize)fd_count);
        int *fds = (int *)CMSG_DATA(cmsg);
        for (int i = 0; i < fd_count; i++) {
            fds[i] = g_unix_fd_list_get(fd_list, i, &error);
            if (fds[i] < 0) {
                g_warning("bridge %s fd-list lookup index=%d failed: %s", c->unique, i,
                          error ? error->message : "unknown error");
                g_clear_error(&error);
                for (int j = 0; j < i; j++) close(fds[j]);
                g_free(control); g_free(blob); return FALSE;
            }
        }
    }
    ssize_t sent = sendmsg(c->bridge_fd, &msg, MSG_NOSIGNAL);
    int saved_errno = errno;
    for (int i = 0; i < fd_count; i++) close(((int *)CMSG_DATA(CMSG_FIRSTHDR(&msg)))[i]);
    g_free(control); g_free(blob);
    if (sent < 0 || (gsize)sent != size) {
        g_warning("bridge %s sendmsg fd=%d failed: sent=%zd expected=%zu errno=%d (%s)",
                  c->unique ? c->unique : "<unregistered>", c->bridge_fd, sent, size,
                  saved_errno, g_strerror(saved_errno));
        return FALSE;
    }
    return TRUE;
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
    if (c->bridge) {
        bridge_send_message(c, m);
        return;
    }
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
static gboolean client_is_live(Broker *b, Client *client) {
    for (guint i = 0; i < b->clients->len; i++)
        if (g_ptr_array_index(b->clients, i) == client) return TRUE;
    return FALSE;
}
static void remove_client_from_queues(Broker *b, Client *client) {
    GHashTableIter iter; gpointer key, value;
    g_hash_table_iter_init(&iter, b->name_queues);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GQueue *queue = value;
        g_queue_remove_all(queue, client);
        if (g_queue_is_empty(queue)) g_hash_table_iter_remove(&iter);
    }
}
static void promote_queued_name(Broker *b, const char *name, const char *old_owner) {
    GQueue *queue = g_hash_table_lookup(b->name_queues, name);
    while (queue && !g_queue_is_empty(queue)) {
        Client *next = g_queue_pop_head(queue);
        if (!client_is_live(b, next)) continue;
        g_hash_table_insert(b->owners, g_strdup(name), next);
        g_hash_table_insert(b->name_flags, g_strdup(name), GUINT_TO_POINTER(0));
        emit_name(next, "NameAcquired", name);
        emit_changed(b, name, old_owner, next->unique);
        if (g_queue_is_empty(queue)) g_hash_table_remove(b->name_queues, name);
        return;
    }
    if (queue) g_hash_table_remove(b->name_queues, name);
    emit_changed(b, name, old_owner, NULL);
}
static gboolean queue_name(Broker *b, const char *name, Client *client) {
    GQueue *queue = g_hash_table_lookup(b->name_queues, name);
    if (!queue) {
        queue = g_queue_new();
        g_hash_table_insert(b->name_queues, g_strdup(name), queue);
    }
    if (g_queue_find(queue, client)) return FALSE;
    g_queue_push_tail(queue, client);
    return TRUE;
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

static GDBusMessage *bus_reply(GDBusMessage *call, GVariant *body) {
    GDBusMessage *reply = g_dbus_message_new_method_reply(call);
    if (body != NULL) g_dbus_message_set_body(reply, body);
    return reply;
}
static GDBusMessage *bus_error(GDBusMessage *call, const char *name, const char *message) {
    return g_dbus_message_new_method_error_literal(call, name, message);
}
static GDBusMessage *bus_method_message(Client *c, GDBusMessage *call) {
    Broker *b = c->broker;
    const char *interface = g_dbus_message_get_interface(call);
    const char *method = g_dbus_message_get_member(call);
    GVariant *parameters = g_dbus_message_get_body(call);
    if (parameters == NULL) parameters = g_variant_new_tuple(NULL, 0);
    if (interface == NULL || method == NULL)
        return bus_error(call, "org.freedesktop.DBus.Error.UnknownMethod", "Missing bus method interface or member");
    const char *signature = expected_signature(interface, method);
    if (signature != NULL && !g_variant_is_of_type(parameters, G_VARIANT_TYPE(signature))) {
        return bus_error(call, "org.freedesktop.DBus.Error.InvalidArgs", "Method body has the wrong signature");
    }
    if (g_str_equal(interface, "org.freedesktop.DBus.Peer")) {
        if (g_str_equal(method, "Ping")) return bus_reply(call, NULL);
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
            GDBusMessage *reply = bus_reply(call, g_variant_new("(s)", id)); g_free(id); return reply;
        } else return bus_error(call, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown Peer method");
        return bus_reply(call, NULL);
    }
    if (g_str_equal(interface, "org.freedesktop.DBus.Introspectable")) {
        if (g_str_equal(method, "Introspect"))
            return bus_reply(call, g_variant_new("(s)", dbus_xml));
        return bus_error(call, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown introspection method");
    }
    if (g_str_equal(method, "Hello")) {
        if (c->unique != NULL) {
            return bus_error(call, "org.freedesktop.DBus.Error.AlreadyRegistered", "Connection already said Hello");
        }
        c->unique = g_strdup_printf(":1.%u", b->next_id++);
        return bus_reply(call, g_variant_new("(s)", c->unique));
    } else if (g_str_equal(method, "RequestName")) {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(su)"))) { return bus_error(call, "org.freedesktop.DBus.Error.InvalidArgs", "Expected (su)"); }
        const char *name; guint flags; g_variant_get(parameters, "(&su)", &name, &flags);
        if (c->unique == NULL || !g_dbus_is_name(name) || name[0] == ':' ||
            g_str_equal(name, "org.freedesktop.DBus") || (flags & ~7u) != 0) {
            return bus_error(call, "org.freedesktop.DBus.Error.InvalidArgs", "Invalid bus name");
        }
        /* The upstream per-stub Flatpak portal owns this one exact reserved
         * portal name. It is granted explicitly by that stub's bus policy;
         * wildcard app ownership remains unable to claim portal names. */
        if (name_is_denied(name) || !(access_for(b, name) & ACCESS_OWN)) {
            return bus_error(call, "org.freedesktop.DBus.Error.AccessDenied", "Name is denied by broker policy");
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
                return bus_reply(call, g_variant_new("(u)", 1u));
            } else {
                if (!(flags & 4u)) {
                    queue_name(b, name, c);
                    return bus_reply(call, g_variant_new("(u)", 2u));
                }
                return bus_reply(call, g_variant_new("(u)", 3u));
            }
        } else if (old_client == c) return bus_reply(call, g_variant_new("(u)", 4u));
        else {
            g_hash_table_insert(b->owners, g_strdup(name), c);
            g_hash_table_insert(b->name_flags, g_strdup(name), GUINT_TO_POINTER(flags));
            emit_name(c, "NameAcquired", name); emit_changed(b, name, NULL, c->unique);
            return bus_reply(call, g_variant_new("(u)", 1u));
        }
    } else if (g_str_equal(method, "ReleaseName")) {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(s)"))) { return bus_error(call, "org.freedesktop.DBus.Error.InvalidArgs", "Expected (s)"); }
        const char *name; g_variant_get(parameters, "(&s)", &name);
        Client *owner = g_hash_table_lookup(b->owners, name);
        if (!g_hash_table_contains(b->owners, name)) {
            GQueue *queue = g_hash_table_lookup(b->name_queues, name);
            if (queue && g_queue_remove(queue, c)) {
                if (g_queue_is_empty(queue)) g_hash_table_remove(b->name_queues, name);
                return bus_reply(call, g_variant_new("(u)", 1u));
            }
            return bus_reply(call, g_variant_new("(u)", 3u));
        } else if (owner != c) return bus_reply(call, g_variant_new("(u)", 2u));
        else {
            char *old = g_strdup(c->unique); g_hash_table_remove(b->owners, name);
            g_hash_table_remove(b->name_flags, name);
            emit_name(c, "NameLost", name); promote_queued_name(b, name, old); g_free(old);
            return bus_reply(call, g_variant_new("(u)", 1u));
        }
    } else if (g_str_equal(method, "GetNameOwner") || g_str_equal(method, "NameHasOwner")) {
        const char *name; g_variant_get(parameters, "(&s)", &name); const char *owner = owner_of(b, name);
        if (g_str_equal(method, "NameHasOwner")) return bus_reply(call, g_variant_new("(b)", owner != NULL));
        else if (owner == NULL) return bus_error(call, "org.freedesktop.DBus.Error.NameHasNoOwner", "Name has no owner");
        else return bus_reply(call, g_variant_new("(s)", owner));
    } else if (g_str_equal(method, "ListNames")) {
        GVariantBuilder a; g_variant_builder_init(&a, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&a, "s", "org.freedesktop.DBus");
        if (b->services->len != 0) g_variant_builder_add(&a, "s", ":1.0");
        for (guint i = 0; i < b->clients->len; i++) {
            Client *x = g_ptr_array_index(b->clients, i); if (x->unique) g_variant_builder_add(&a, "s", x->unique);
        }
        GHashTableIter it; gpointer key; g_hash_table_iter_init(&it, b->owners);
        while (g_hash_table_iter_next(&it, &key, NULL)) g_variant_builder_add(&a, "s", (char *)key);
        return bus_reply(call, g_variant_new("(as)", &a));
    } else if (g_str_equal(method, "ListActivatableNames")) {
        GVariantBuilder a; g_variant_builder_init(&a, G_VARIANT_TYPE("as"));
        return bus_reply(call, g_variant_new("(as)", &a));
    } else if (g_str_equal(method, "StartServiceByName")) {
        const char *name; guint flags; g_variant_get(parameters, "(&su)", &name, &flags);
        if (!g_dbus_is_name(name) || name[0] == ':' || flags != 0) {
            return bus_error(call, "org.freedesktop.DBus.Error.InvalidArgs", "Invalid activation request");
        }
        if ((name_is_denied(name) && !(g_hash_table_contains(b->owners,name) && g_hash_table_lookup(b->owners,name)==NULL)) || !(access_for(b, name) & ACCESS_TALK)) {
            /* Nothing to activate: a service MatonOS does not provide is
             * absent (apps treat ServiceUnknown as "not installed"; an
             * AccessDenied surfaced as a LibreOffice error dialog). An
             * existing but disallowed one is still denied. */
            if (owner_of(b, name) == NULL)
                return bus_error(call, "org.freedesktop.DBus.Error.ServiceUnknown", "The name is not provided by any service");
            else
                return bus_error(call, "org.freedesktop.DBus.Error.AccessDenied", "Activation is denied by broker policy");
        }
        if (owner_of(b, name) != NULL)
            return bus_reply(call, g_variant_new("(u)", 2u));
        else
            return bus_error(call, "org.freedesktop.DBus.Error.ServiceUnknown", "No activatable service is configured");
    } else if (g_str_equal(method, "AddMatch") || g_str_equal(method, "RemoveMatch")) {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(s)"))) { return bus_error(call, "org.freedesktop.DBus.Error.InvalidArgs", "Expected (s)"); }
        const char *rule; g_variant_get(parameters, "(&s)", &rule);
        if (strlen(rule) > 4096) { return bus_error(call, "org.freedesktop.DBus.Error.MatchRuleInvalid", "Match rule too long"); }
        /* This broker implements only these keys. Reject rules it cannot honor. */
        if (!match_rule_supported(rule)) {
            return bus_error(call, "org.freedesktop.DBus.Error.MatchRuleInvalid", "Unsupported match rule key");
        }
        gboolean add = g_str_equal(method, "AddMatch");
        if (add) {
            if (c->matches->len >= MAX_MATCHES_PER_CLIENT) { return bus_error(call, "org.freedesktop.DBus.Error.LimitsExceeded", "Too many match rules"); }
            g_ptr_array_add(c->matches, g_strdup(rule));
        }
        else {
            gboolean removed = FALSE;
            for (guint i = 0; i < c->matches->len; i++) if (g_str_equal(rule, g_ptr_array_index(c->matches, i))) {
                g_ptr_array_remove_index(c->matches, i); removed = TRUE; break;
            }
            if (!removed) { return bus_error(call, "org.freedesktop.DBus.Error.MatchRuleNotFound", "No matching rule"); }
        }
        return bus_reply(call, NULL);
    } else if (g_str_equal(method,"GetConnectionCredentials")) {
        const char* name;g_variant_get(parameters,"(&s)",&name);
        const char* unique=owner_of(b,name);
        Client* target=unique ? find_client(b,unique) : NULL;
        if (target && target->bridge)
            return bus_error(call,"org.freedesktop.DBus.Error.Failed","bridge peer has no Unix credentials");
        GCredentials* credentials=target && target->connection ? g_dbus_connection_get_peer_credentials(target->connection) : NULL;
        gboolean internal=g_strcmp0(unique,":1.0")==0;
        if (!credentials && !internal) return bus_error(call,"org.freedesktop.DBus.Error.NameHasNoOwner","Peer credentials are unavailable");
        GError* ce=NULL;uid_t uid=internal ? b->owner_uid : g_credentials_get_unix_user(credentials,&ce);
        pid_t pid=internal ? getpid() : g_credentials_get_unix_pid(credentials,NULL);
        if (ce || pid<=0) {g_clear_error(&ce);return bus_error(call,"org.freedesktop.DBus.Error.Failed","Peer credentials are unavailable");}
        GVariantBuilder result;g_variant_builder_init(&result,G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&result,"{sv}","UnixUserID",g_variant_new_uint32(uid));
        g_variant_builder_add(&result,"{sv}","ProcessID",g_variant_new_uint32(pid));
        return bus_reply(call,g_variant_new("(a{sv})",&result));
    } else if (g_str_equal(method, "GetConnectionUnixUser") || g_str_equal(method, "GetConnectionUnixProcessID")) {
        const char *name; g_variant_get(parameters, "(&s)", &name);
        if (g_str_equal(name, "org.freedesktop.DBus")) { guint value=g_str_equal(method,"GetConnectionUnixUser") ? (guint)b->owner_uid : (guint)getpid();return bus_reply(call, g_variant_new("(u)", value)); }
        if (g_str_equal(name, ":1.0")) {
            guint value = g_str_equal(method, "GetConnectionUnixUser") ? (guint)b->owner_uid : (guint)getpid();
            return bus_reply(call, g_variant_new("(u)", value));
        }
        const char *unique = owner_of(b, name);
        if (g_strcmp0(unique, ":1.0") == 0) {
            guint value = g_str_equal(method, "GetConnectionUnixUser") ? (guint)b->owner_uid : (guint)getpid();
            return bus_reply(call, g_variant_new("(u)", value));
        }
        Client *target = unique ? find_client(b, unique) : NULL;
        if (target == NULL) { return bus_error(call, "org.freedesktop.DBus.Error.NameHasNoOwner", "Connection does not exist"); }
        if (target->bridge)
            return bus_error(call, "org.freedesktop.DBus.Error.Failed", "bridge peer has no Unix credentials");
        GCredentials *credentials = g_dbus_connection_get_peer_credentials(target->connection);
        guint value = (guint)b->owner_uid;
        if (g_str_equal(method, "GetConnectionUnixProcessID")) {
            if (credentials == NULL) { return bus_error(call, "org.freedesktop.DBus.Error.Failed", "Peer process credentials are unavailable"); }
            pid_t pid = g_credentials_get_unix_pid(credentials, NULL);
            if (pid <= 0) { return bus_error(call, "org.freedesktop.DBus.Error.Failed", "Peer process ID is unavailable"); }
            value = (guint)pid;
        } else if (credentials != NULL) {
            GError *ce = NULL; uid_t uid = g_credentials_get_unix_user(credentials, &ce);
            if (ce == NULL) value = (guint)uid; else g_clear_error(&ce);
        }
        return bus_reply(call, g_variant_new("(u)", value));
    } else if (g_str_equal(method, "GetId")) {
        return bus_reply(call, g_variant_new("(s)", b->id));
    } else {
        return bus_error(call, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown bus method");
    }
    return NULL;
}

static void bus_method_call(GDBusConnection *connection, const char *sender,
                            const char *path, const char *interface,
                            const char *method, GVariant *parameters,
                            GDBusMethodInvocation *inv, gpointer user_data) {
    (void)sender; (void)path; (void)interface; (void)method; (void)parameters;
    Client *c = user_data;
    GDBusMessage *call = g_dbus_method_invocation_get_message(inv);
    GDBusMessage *reply = bus_method_message(c, call);
    if (reply == NULL) return;
    if (g_dbus_message_get_message_type(reply) == G_DBUS_MESSAGE_TYPE_ERROR) {
        const char *name = g_dbus_message_get_error_name(reply);
        GVariant *body = g_dbus_message_get_body(reply);
        const char *message = "D-Bus method failed";
        if (body != NULL && g_variant_is_of_type(body, G_VARIANT_TYPE("(s)")))
            g_variant_get(body, "(&s)", &message);
        g_dbus_method_invocation_return_dbus_error(inv, name, message);
    } else {
        g_dbus_method_invocation_return_value(inv, g_dbus_message_get_body(reply));
    }
    g_object_unref(reply);
    (void)connection;
}

static const GDBusInterfaceVTable bus_vtable = { .method_call = bus_method_call };

static GDBusMessage *route_message(GDBusConnection *, GDBusMessage *, gboolean, gpointer);
static void client_bridge_closed(Client *c);
static void bridge_bus_call(Client *c, GDBusMessage *m) {
    const char *path = g_dbus_message_get_path(m);
    GDBusMessage *reply;
    if (g_strcmp0(path, "/org/freedesktop/DBus") != 0 && g_strcmp0(path, "/") != 0) {
        reply = bus_error(m, "org.freedesktop.DBus.Error.ServiceUnknown", "The bus driver object has no such path");
    } else {
        reply = bus_method_message(c, m);
    }
    if (reply == NULL) {
        g_warning("bridge %s bus method returned no reply member=%s", c->unique,
                  g_dbus_message_get_member(m) ? g_dbus_message_get_member(m) : "<none>");
        reply = bus_error(m, "org.freedesktop.DBus.Error.Failed", "Bus method returned no reply");
    }
    g_dbus_message_set_destination(reply, c->unique);
    send_raw(c, reply);
    g_object_unref(reply);
}

static void client_bridge_closed(Client *c) {
    Broker *b=c->broker;
    if(c->bridge_source){g_source_remove(c->bridge_source);c->bridge_source=0;}
    for(guint i=0;i<b->clients->len;i++) if(g_ptr_array_index(b->clients,i)==c) {
        remove_client_from_queues(b,c);
        GHashTableIter it; gpointer key,value; GPtrArray *names=g_ptr_array_new_with_free_func(g_free);
        g_hash_table_iter_init(&it,b->owners);
        while(g_hash_table_iter_next(&it,&key,&value)) if(value==c)g_ptr_array_add(names,g_strdup(key));
        for(guint j=0;j<names->len;j++){const char*n=g_ptr_array_index(names,j);g_hash_table_remove(b->owners,n);g_hash_table_remove(b->name_flags,n);promote_queued_name(b,n,c->unique);}
        g_ptr_array_unref(names);
        /* Calls originated by the bridge must be removed from every peer's
         * pending table. Calls addressed to it fail back to their origin. */
        for(guint j=0;j<c->outgoing->len;j++){
            Pending*p=g_ptr_array_index(c->outgoing,j);
            for(guint k=0;k<b->clients->len;k++){
                Client*d=g_ptr_array_index(b->clients,k);GHashTableIter pi;gpointer pk,pv;
                g_hash_table_iter_init(&pi,d->pending);
                while(g_hash_table_iter_next(&pi,&pk,&pv))if(pv==p)g_hash_table_iter_remove(&pi);
            }
        }
        GHashTableIter pending_iter;gpointer pending_key,pending_value;
        g_hash_table_iter_init(&pending_iter,c->pending);
        while(g_hash_table_iter_next(&pending_iter,&pending_key,&pending_value)){
            Pending*p=pending_value;g_ptr_array_remove(p->origin->outgoing,p);
            GDBusMessage*request=g_dbus_message_new();
            g_dbus_message_set_message_type(request,G_DBUS_MESSAGE_TYPE_METHOD_CALL);
            g_dbus_message_set_serial(request,p->serial);
            GDBusMessage*failure=g_dbus_message_new_method_error_literal(request,
                "org.freedesktop.DBus.Error.ServiceUnknown","Bridge provider disconnected");
            send_raw(p->origin,failure);g_object_unref(failure);g_object_unref(request);
        }
        g_hash_table_remove_all(c->pending);
        g_ptr_array_set_size(c->outgoing,0);
        g_ptr_array_remove_index(b->clients,i);break;
    }
    g_warning("bridge %s closed; releasing client state",c->unique?c->unique:"<unregistered>");
    if(c->bridge_fd>=0){int fd=c->bridge_fd;c->bridge_fd=-1;g_warning("close fd=%d at broker.c bridge disconnect",fd);close(fd);}
    g_hash_table_unref(c->pending);g_ptr_array_unref(c->outgoing);g_ptr_array_unref(c->matches);g_free(c->unique);g_free(c);
}

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
    if(path && (g_str_equal(path,"/org/freedesktop/portal/desktop") ||
                g_str_has_prefix(path,"/org/freedesktop/portal/desktop/")) &&
            (access_for(b,"org.freedesktop.portal.Desktop") & ACCESS_TALK))return TRUE;
    if(g_strcmp0(path,"/org/freedesktop/Flatpak/Development")==0 &&
            (access_for(b,"org.freedesktop.Flatpak") & ACCESS_TALK))return TRUE;
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
    /* MatonOS Development runs inside this app sandbox, via Java. */
    if(g_str_equal(dest,"org.freedesktop.Flatpak") &&
            g_strcmp0(g_dbus_message_get_path(m),"/org/freedesktop/Flatpak/Development")==0 &&
            (g_strcmp0(iface,"org.freedesktop.Flatpak.Development")==0 ||
             g_strcmp0(iface,"org.freedesktop.DBus.Properties")==0 ||
             g_strcmp0(iface,"org.freedesktop.DBus.Introspectable")==0))return FALSE;
    if (name_is_denied(dest)) return TRUE;
    if ((g_str_equal(dest, "org.freedesktop.PackageKit") || g_str_has_prefix(dest, "org.freedesktop.PackageKit.")) &&
        member != NULL && (g_strrstr(member, "Install") != NULL || g_strrstr(member, "Remove") != NULL)) return TRUE;
    if (iface != NULL && g_str_equal(iface, "org.freedesktop.Flatpak")) return TRUE;
    return FALSE;
}
/* Portal completion crosses the in-process bus, outside payload stderr.
 * Keep this small breadcrumb on Android too: stderr there is /dev/null. */
static void trace_flatpak_message(GDBusMessage *message, const char *peer, gboolean reply) {
    const char *iface=g_dbus_message_get_interface(message);
    const char *member=g_dbus_message_get_member(message);
    if (!reply && g_strcmp0(iface,"org.freedesktop.portal.Flatpak")!=0) return;
    GVariant *body=g_dbus_message_get_body(message);
    char *args=body?g_variant_print(body,TRUE):g_strdup("()");
    const char *destination=g_dbus_message_get_destination(message);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO,"MatonPortalBus",
#else
    g_printerr(
#endif
        "Flatpak peer=%s destination=%s serial=%u reply_serial=%u member=%s args=%s\n",
        peer?peer:"<pre-Hello>",destination?destination:"<broadcast>",
        g_dbus_message_get_serial(message),g_dbus_message_get_reply_serial(message),member?member:(g_dbus_message_get_error_name(message)?g_dbus_message_get_error_name(message):"<reply>"),args);
    g_free(args);
}
static GDBusMessage *route_message(GDBusConnection *connection, GDBusMessage *message,
                                   gboolean incoming, gpointer data) {
    (void)connection;
    Client *c = data; Broker *b = c->broker;
    if (!incoming) return message;
    GDBusMessageType type = g_dbus_message_get_message_type(message);
    trace_flatpak_message(message,c->unique,FALSE);
    if (type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN || type == G_DBUS_MESSAGE_TYPE_ERROR) {
        guint32 reply = g_dbus_message_get_reply_serial(message);
        Pending *p = g_hash_table_lookup(c->pending, GUINT_TO_POINTER(reply));
        if (p != NULL) {
            if(p->flatpak)trace_flatpak_message(message,c->unique,TRUE);
            GDBusMessage *forward;
            forward = g_dbus_message_copy(message, NULL);
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
            /* A private session bus belongs to one app. Directed signals to
             * a live unique connection are addressed to that app directly;
             * unique names do not appear in the well-known owner table used
             * by destination_has_talk_access(). In particular, the Flatpak
             * portal sends SpawnExited to its caller's unique name. */
            gboolean allowed = destination[0] == ':' ? target != NULL :
                ((access_for(b, destination) & ACCESS_TALK)!=0);
            if (target && allowed) send_raw(target,routed);
            trace_flatpak_message(routed,target&&allowed?"routed-to-live-client":"undeliverable",FALSE);
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
        if ((g_str_equal(dest,"org.freedesktop.portal.Desktop") ||
                g_str_equal(dest,"org.freedesktop.Flatpak")) && broker_portal_forward(b,connection,message,c->unique)) return NULL;
        return message;
    }
    if (g_str_equal(dest, ":1.0") && b->services->len != 0) {
        if (!internal_service_has_talk_access(b, message)) {
            GDBusMessage *err = g_dbus_message_new_method_error_literal(message,
                "org.freedesktop.DBus.Error.AccessDenied", "Destination is denied by broker policy");
            send_raw(c, err); g_object_unref(err); return NULL;
        }
        if (broker_portal_forward(b,connection,message,c->unique)) return NULL;
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
    gboolean sent = FALSE;
    if (target->bridge) {
        forwarded = b->next_id++;
        g_dbus_message_set_serial(copy, forwarded);
        sent = bridge_send_message(target, copy);
    } else sent = g_dbus_connection_send_message(target->connection, copy,
            G_DBUS_SEND_MESSAGE_FLAGS_NONE, &forwarded, &error);
    if (sent) {
        Pending *p = g_new0(Pending, 1); p->origin = c; p->serial = g_dbus_message_get_serial(message);
        p->flatpak=g_strcmp0(g_dbus_message_get_interface(message),"org.freedesktop.portal.Flatpak")==0;
        g_ptr_array_add(c->outgoing, p);
        g_hash_table_insert(target->pending, GUINT_TO_POINTER(forwarded), p);
    } else {
        GDBusMessage *err = g_dbus_message_new_method_error_literal(message,
            "org.freedesktop.DBus.Error.ServiceUnknown", error ? error->message : "Destination unavailable");
        g_warning("cannot route method call from=%s to=%s: %s", c->unique ? c->unique : "<unregistered>",
                  target->unique ? target->unique : target_name,
                  error ? error->message : "bridge send failed");
        send_raw(c, err); g_object_unref(err); g_clear_error(&error);
    }
    g_object_unref(copy);
    return NULL;
}

/* GDBus passes the filter ownership of message (transfer full). route_message
 * forwards copies and returns NULL to drop the original, so release it here:
 * otherwise every routed message leaks, and with it any SCM_RIGHTS fds it
 * carried (a Spawn's stdio pipes then never reach EOF in the caller). */
static GDBusMessage *filter_message(GDBusConnection *connection, GDBusMessage *message,
                                    gboolean incoming, gpointer data) {
    GDBusMessage *result = route_message(connection, message, incoming, data);
    if (result != message) g_object_unref(message);
    return result;
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
        remove_client_from_queues(b,c);
        GHashTableIter iter; gpointer key, value; GPtrArray *gone = g_ptr_array_new_with_free_func(g_free);
        g_hash_table_iter_init(&iter, b->owners);
        while (g_hash_table_iter_next(&iter, &key, &value)) if (value == c) g_ptr_array_add(gone, g_strdup(key));
        for (guint j = 0; j < gone->len; j++) {
            const char *name = g_ptr_array_index(gone, j); g_hash_table_remove(b->owners, name);
            g_hash_table_remove(b->name_flags, name);
            emit_name(c, "NameLost", name); promote_queued_name(b, name, c->unique);
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
    c->bridge_fd = -1;
    GIOStream *stream = g_dbus_connection_get_stream(connection);
    if (!G_IS_SOCKET_CONNECTION(stream)) { g_object_unref(c->connection); g_free(c); return FALSE; }
    int fd = g_socket_get_fd(g_socket_connection_get_socket(G_SOCKET_CONNECTION(stream)));
    struct ucred peercred; socklen_t peercred_len = sizeof(peercred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peercred, &peercred_len) != 0 ||
        peercred.uid != b->owner_uid) {
        g_object_unref(c->connection); g_free(c); return FALSE;
    }
    GCredentials *peer = g_dbus_connection_get_peer_credentials(connection);
    if (peer == NULL) { g_object_unref(c->connection); g_free(c); return FALSE; }
    GError *cred_error = NULL;
    uid_t peer_uid = g_credentials_get_unix_user(peer, &cred_error);
    if (cred_error != NULL || peer_uid != b->owner_uid) {
        g_clear_error(&cred_error); g_object_unref(c->connection); g_free(c); return FALSE;
    }
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

static gboolean bridge_readable(gint fd, GIOCondition condition, gpointer data) {
    Client *c = data;
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        g_warning("bridge %s fd=%d disconnected condition=0x%x", c->unique ? c->unique : "<unregistered>", fd, condition);
        /* The source is removed before cleanup so no callback can observe freed state. */
        c->bridge_source = 0;
        client_bridge_closed(c);
        return G_SOURCE_REMOVE;
    }
    ssize_t size = recv(fd, NULL, 0, MSG_PEEK | MSG_TRUNC | MSG_DONTWAIT);
    if (size < 0) {
        if (errno == EAGAIN || errno == EINTR) return G_SOURCE_CONTINUE;
        g_warning("bridge %s fd=%d datagram-size peek failed errno=%d (%s)", c->unique ? c->unique : "<unregistered>", fd, errno, g_strerror(errno));
        c->bridge_source = 0; client_bridge_closed(c); return G_SOURCE_REMOVE;
    }
    if (size == 0) {
        g_warning("bridge %s fd=%d received an empty datagram", c->unique ? c->unique : "<unregistered>", fd);
        return G_SOURCE_CONTINUE;
    }
    guint8 *buffer = g_try_malloc((gsize)size);
    if (buffer == NULL) {
        g_warning("bridge %s fd=%d cannot allocate datagram buffer size=%zd", c->unique ? c->unique : "<unregistered>", fd, size);
        c->bridge_source = 0; client_bridge_closed(c); return G_SOURCE_REMOVE;
    }
    ssize_t peeked = recv(fd, buffer, (gsize)size, MSG_PEEK | MSG_DONTWAIT | MSG_TRUNC);
    if (peeked != size) {
        g_warning("bridge %s fd=%d datagram peek changed expected=%zd actual=%zd errno=%d (%s)", c->unique, fd, size, peeked, errno, g_strerror(errno));
        g_free(buffer); c->bridge_source = 0; client_bridge_closed(c); return G_SOURCE_REMOVE;
    }
    GError *error = NULL;
    GDBusMessage *peek_message = g_dbus_message_new_from_blob(buffer, (gsize)peeked,
            G_DBUS_CAPABILITY_FLAGS_UNIX_FD_PASSING, &error);
    if (!peek_message) {
        g_warning("bridge %s malformed D-Bus datagram fd=%d: %s", c->unique ? c->unique : "<unregistered>", fd, error->message);
        g_clear_error(&error); g_free(buffer); return G_SOURCE_CONTINUE;
    }
    guint declared_fds = g_dbus_message_get_num_unix_fds(peek_message);
    g_object_unref(peek_message);
    gsize control_size = declared_fds ? CMSG_SPACE(sizeof(int) * (gsize)declared_fds) : 0;
    void *control = control_size ? g_try_malloc0(control_size) : NULL;
    if (control_size && !control) {
        g_warning("bridge %s fd=%d cannot allocate SCM_RIGHTS receive buffer declared_fds=%u bytes=%zu; dropping datagram",
                  c->unique ? c->unique : "<unregistered>", fd, declared_fds, control_size);
        g_free(buffer); return G_SOURCE_CONTINUE;
    }
    struct iovec iov = { .iov_base = buffer, .iov_len = (gsize)size };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = control_size };
    ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_TRUNC | MSG_CMSG_CLOEXEC);
    if (n != size) {
        g_warning("bridge %s fd=%d datagram read size changed expected=%zd actual=%zd errno=%d (%s)", c->unique ? c->unique : "<unregistered>", fd, size, n, errno, g_strerror(errno));
        g_free(control); g_free(buffer); c->bridge_source = 0; client_bridge_closed(c); return G_SOURCE_REMOVE;
    }
    GUnixFDList *fd_list = g_unix_fd_list_new();
    guint received_fds = 0;
    gboolean ancillary_valid = !(msg.msg_flags & MSG_CTRUNC);
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
                cmsg->cmsg_len < CMSG_LEN(0) ||
                (cmsg->cmsg_len - CMSG_LEN(0)) % sizeof(int) != 0) {
            ancillary_valid = FALSE; continue;
        }
        guint count = (guint)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        int *fds = (int *)CMSG_DATA(cmsg);
        for (guint i = 0; i < count; i++) {
            if (g_unix_fd_list_append(fd_list, fds[i], &error) < 0) {
                g_warning("bridge %s fd=%d received fd append failed: %s", c->unique, fd, error->message);
                g_clear_error(&error); ancillary_valid = FALSE;
            }
            close(fds[i]);
            received_fds++;
        }
    }
    g_free(control);
    GDBusMessage *message = g_dbus_message_new_from_blob(buffer, (gsize)n,
            G_DBUS_CAPABILITY_FLAGS_UNIX_FD_PASSING, &error);
    g_free(buffer);
    if (message == NULL) {
        g_warning("bridge %s malformed D-Bus datagram fd=%d: %s", c->unique ? c->unique : "<unregistered>", fd, error->message);
        g_clear_error(&error); g_object_unref(fd_list); return G_SOURCE_CONTINUE;
    }
    guint message_fds = g_dbus_message_get_num_unix_fds(message);
    if (!ancillary_valid || received_fds != message_fds) {
        g_warning("bridge %s fd=%d UNIX_FDS mismatch declared=%u received=%u flags=0x%x; dropping datagram",
                  c->unique, fd, message_fds, received_fds, msg.msg_flags);
        g_object_unref(fd_list); g_object_unref(message); return G_SOURCE_CONTINUE;
    }
    if (received_fds) g_dbus_message_set_unix_fd_list(message, fd_list);
    g_object_unref(fd_list);
    g_dbus_message_set_sender(message, c->unique);
    /* Bridge bus-driver messages enter the same handler used by real clients;
     * all other traffic enters the normal router. */
    if (g_dbus_message_get_message_type(message) == G_DBUS_MESSAGE_TYPE_METHOD_CALL &&
        g_strcmp0(g_dbus_message_get_destination(message), "org.freedesktop.DBus") == 0) {
        bridge_bus_call(c, message);
    } else {
        route_message(NULL, message, TRUE, c);
    }
    g_object_unref(message);
    return G_SOURCE_CONTINUE;
}

Broker *broker_new(const char *socket_path, const char *config_path, GError **error) {
    Broker *b = g_new0(Broker, 1); b->socket_path = g_strdup(socket_path); b->owner_uid = geteuid();
    b->clients = g_ptr_array_new(); b->services = g_ptr_array_new();
    b->owners = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    b->name_flags = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    b->name_queues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_queue_free);
    b->rules = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL); b->next_id = 1;
    b->trace_calls = g_strcmp0(g_getenv("MATONOS_DBUS_TRACE"), "1") == 0;
    b->id = g_dbus_generate_guid();
    gchar *contents = NULL; gsize length = 0;
    if (!g_file_get_contents(config_path, &contents, &length, error)) { broker_free(b); return NULL; }
    char **lines = g_strsplit(contents, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        char *line = g_strstrip(lines[i]); if (!*line || *line == '#') continue;
        char **p = g_strsplit_set(line, " \t", 2);
        if (p[0] == NULL || p[1] == NULL || (!g_dbus_is_name(g_strstrip(p[1])) && strcmp(p[1], "*"))) {
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

gboolean broker_add_bridge(Broker *b, int fd, const char *const *names, GError **error) {
    if(fd<0 || names==NULL){g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT,"bridge requires fd and names");return FALSE;}
    Client*c=g_new0(Client,1);c->broker=b;c->bridge=TRUE;c->bridge_fd=fd;
    c->matches=g_ptr_array_new_with_free_func(g_free);c->outgoing=g_ptr_array_new();
    c->pending=g_hash_table_new_full(g_direct_hash,g_direct_equal,NULL,g_free);
    c->unique=g_strdup_printf(":1.%u",b->next_id++);
    g_ptr_array_add(b->clients,c);
    for(guint i=0;names[i];i++){
        if(!g_dbus_is_name(names[i])||names[i][0]==':'||name_is_reserved(names[i])){
            g_warning("bridge %s rejected invalid/reserved name '%s'",c->unique,names[i]);continue;
        }
        if(owner_of(b,names[i])){g_warning("bridge %s queued for already-owned name '%s'",c->unique,names[i]);queue_name(b,names[i],c);continue;}
        g_hash_table_insert(b->owners,g_strdup(names[i]),c);
        emit_name(c,"NameAcquired",names[i]);emit_changed(b,names[i],NULL,c->unique);
    }
    c->bridge_source=g_unix_fd_add_full(G_PRIORITY_DEFAULT,fd,G_IO_IN|G_IO_HUP|G_IO_ERR|G_IO_NVAL,bridge_readable,c,NULL);
    if(!c->bridge_source){g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,"cannot create bridge fd source");client_bridge_closed(c);return FALSE;}
    return TRUE;
}
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
    b->server = g_dbus_server_new_sync(address, G_DBUS_SERVER_FLAGS_AUTHENTICATION_REQUIRE_SAME_USER,
                                        guid, NULL, NULL, error);
    g_free(address); g_free(guid);
    if (b->server == NULL) return FALSE;
    if (chmod(b->socket_path, 0600) != 0) {
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
    if (b->server) { g_dbus_server_stop(b->server); g_object_unref(b->server); }
    if (b->loop) g_main_loop_unref(b->loop);
    if (b->clients) {
        while (b->clients->len) {
            Client *c = g_ptr_array_index(b->clients, b->clients->len - 1);
            if(c->bridge){ if(c->bridge_source)g_source_remove(c->bridge_source); if(c->bridge_fd>=0){int fd=c->bridge_fd;g_warning("close fd=%d at broker.c shutdown",fd);close(fd);} g_ptr_array_remove_index(b->clients,b->clients->len-1);g_hash_table_unref(c->pending);g_ptr_array_unref(c->outgoing);g_ptr_array_unref(c->matches);g_free(c->unique);g_free(c);continue; }
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
    if (b->name_queues) g_hash_table_unref(b->name_queues);
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

void broker_portal_signal(Broker* b,GDBusMessage* message) {
    const char* dest=g_dbus_message_get_destination(message);
    Client* c=dest ? find_client(b,dest) : NULL;
    if(c){g_dbus_message_set_sender(message,":1.0");send_raw(c,message);}
}
