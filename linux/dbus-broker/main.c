#include "broker.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

gboolean broker_register_example(Broker *broker, GError **error);

int main(int argc, char **argv) {
    gboolean test_example = argc == 4 && g_str_equal(argv[3], "--example");
    gboolean session = argc == 7 && g_str_equal(argv[3],"--flatpak-session");
    if (argc != 3 && !test_example && !session) {
        fprintf(stderr, "usage: %s SOCKET_PATH POLICY_FILE [--example]\n", argv[0]);
        return 2;
    }
    GError *error = NULL;
    Broker *broker = broker_new(argv[1], argv[2], &error);
    if (broker && session) {
        int hold_fd=atoi(argv[6]);
        /* Never let flatpak-portal inherit this lifetime-bound Android hold. */
        if(hold_fd>=0 && fcntl(hold_fd,F_SETFD,FD_CLOEXEC)<0) {broker_free(broker);return 1;}
        broker_enable_flatpak_portal(broker,atoi(argv[5]));
        if (!broker_register_session_services(broker,argv[4],hold_fd,&error)) {broker_free(broker);broker=NULL;}
    }
    if (broker == NULL || (test_example && !broker_register_example(broker, &error)) ||
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
