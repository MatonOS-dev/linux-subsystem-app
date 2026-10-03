#include "broker.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <glib-unix.h>
#include <sys/prctl.h>
#include <signal.h>

gboolean broker_register_example(Broker *broker, GError **error);

static gboolean stopped(gpointer data) {broker_stop(data);return G_SOURCE_CONTINUE;}

int main(int argc, char **argv) {
    gboolean test_example = argc == 4 && g_str_equal(argv[3], "--example");
    gboolean session = argc == 4 && g_str_equal(argv[3],"--host-session");
    if (argc != 3 && !test_example && !session) {
        fprintf(stderr, "usage: %s SOCKET_PATH POLICY_FILE [--example|--host-session]\n", argv[0]);
        return 2;
    }
    if(session && (prctl(PR_SET_PDEATHSIG,SIGTERM) || getppid()==1))return 1;
    GError *error = NULL;
    Broker *broker = broker_new(argv[1], argv[2], &error);
    if (broker && session) {
        struct sockaddr_un address={.sun_family=AF_UNIX};
        char* parent=g_path_get_dirname(argv[1]);
        char* path=g_build_filename(parent,"inhibit",NULL);
        int hold_fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        if(strlen(path)>=sizeof(address.sun_path)) {close(hold_fd);hold_fd=-1;}
        else {strcpy(address.sun_path,path);if(hold_fd>=0 && connect(hold_fd,(struct sockaddr*)&address,sizeof(address))) {close(hold_fd);hold_fd=-1;}}
        g_free(path);g_free(parent);
        if (!broker_enable_host_session(broker,&error) ||
            !broker_register_session_services(broker,NULL,hold_fd,&error)) {broker_free(broker);broker=NULL;}
    }
    guint term=broker ? g_unix_signal_add(SIGTERM,stopped,broker) : 0;
    guint interrupt=broker ? g_unix_signal_add(SIGINT,stopped,broker) : 0;
    if (broker == NULL || (test_example && !broker_register_example(broker, &error)) ||
        !broker_run(broker, &error)) {
        fprintf(stderr, "matonos-dbus-broker: %s\n",
                error != NULL ? error->message : "startup failed");
        g_clear_error(&error);
        if(term)g_source_remove(term);
        if(interrupt)g_source_remove(interrupt);
        broker_free(broker);
        return 1;
    }
    if(term)g_source_remove(term);
    if(interrupt)g_source_remove(interrupt);
    broker_free(broker);
    return 0;
}
