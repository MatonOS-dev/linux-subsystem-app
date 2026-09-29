#include "broker.h"

#include <stdio.h>

gboolean broker_register_example(Broker *broker, GError **error);

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s SOCKET_PATH POLICY_FILE\n", argv[0]);
        return 2;
    }
    GError *error = NULL;
    Broker *broker = broker_new(argv[1], argv[2], &error);
    if (broker == NULL || !broker_register_example(broker, &error) ||
        !broker_run(broker, &error)) {
        fprintf(stderr, "matonos-dbus-broker: %s\n",
                error != NULL ? error->message : "startup failed");
        g_clear_error(&error);
        broker_free(broker);
        return 1;
    }
    broker_free(broker);
    return 0;
}
