#ifndef MATONOS_DBUS_BROKER_H
#define MATONOS_DBUS_BROKER_H

#include <gio/gio.h>
#include <sys/types.h>

typedef struct _Broker Broker;

/* Register an in-process D-Bus object before starting the socket listener. */
gboolean broker_add_service(Broker *broker, const char *name,
                            const char *path, GDBusInterfaceInfo *interface,
                            const GDBusInterfaceVTable *vtable,
                            gpointer user_data, GError **error);
Broker *broker_new(const char *socket_path, const char *config_path,
                   GError **error);
gboolean broker_enable_host_session(Broker *broker, GError **error);
gboolean broker_enable_host_session_for_uid(Broker *broker, uid_t uid, GError **error);
const char* broker_monitor_path(Broker *broker);
void broker_set_monitor_path(Broker *broker, const char *path);
gboolean broker_register_session_services(Broker *broker, const char *monitor, int backend_fd, GError **error);
gboolean broker_run(Broker *broker, GError **error);
void broker_free(Broker *broker);
void broker_stop(Broker *broker);
const char* broker_connection_name(Broker* broker, GDBusConnection* connection);
void* broker_session_data(Broker* broker);
void broker_set_session_data(Broker* broker, void* data);
void broker_session_client_closed(Broker* broker, GDBusConnection* connection);
void broker_session_services_free(Broker* broker);
gboolean broker_portal_forward(Broker* broker, GDBusConnection* connection, GDBusMessage* message, const char* owner);
void broker_portal_signal(Broker* broker, GDBusMessage* message);

#endif
