#ifndef MATONOS_DBUS_BROKER_H
#define MATONOS_DBUS_BROKER_H

#include <gio/gio.h>

typedef struct _Broker Broker;

/* Register an in-process D-Bus object before starting the socket listener. */
gboolean broker_add_service(Broker *broker, const char *name,
                            const char *path, GDBusInterfaceInfo *interface,
                            const GDBusInterfaceVTable *vtable,
                            gpointer user_data, GError **error);
Broker *broker_new(const char *socket_path, const char *config_path,
                   GError **error);
void broker_enable_flatpak_portal(Broker *broker, int ready_fd);
gboolean broker_register_session_services(Broker *broker, const char *monitor, int hold_fd, GError **error);
gboolean broker_run(Broker *broker, GError **error);
void broker_free(Broker *broker);
const char* broker_connection_name(Broker* broker, GDBusConnection* connection);
void* broker_session_data(Broker* broker);
void broker_set_session_data(Broker* broker, void* data);
void broker_session_client_closed(Broker* broker, GDBusConnection* connection);
void broker_session_services_free(Broker* broker);

#endif
