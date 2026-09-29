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
gboolean broker_run(Broker *broker, GError **error);
void broker_free(Broker *broker);

#endif
