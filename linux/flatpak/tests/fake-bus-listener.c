#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static int listener_fd = -1;

static void stop_listener(int signal_number) {
    (void)signal_number;
    stopped = 1;
    if (listener_fd >= 0) close(listener_fd);
}

int main(int argc, char** argv) {
    if (argc != 2 || strlen(argv[1]) >= sizeof(((struct sockaddr_un*)0)->sun_path))
        return 2;
    listener_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener_fd < 0) return 1;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, argv[1]);
    unlink(address.sun_path);
    if (bind(listener_fd, (struct sockaddr*)&address, sizeof(address)) || listen(listener_fd, 8)) {
        close(listener_fd);
        return 1;
    }
    signal(SIGTERM, stop_listener);
    signal(SIGINT, stop_listener);
    while (!stopped) {
        int peer = accept4(listener_fd, NULL, NULL, SOCK_CLOEXEC);
        if (peer >= 0) close(peer);
        else if (errno != EINTR) break;
    }
    if (listener_fd >= 0) close(listener_fd);
    unlink(address.sun_path);
    return 0;
}
