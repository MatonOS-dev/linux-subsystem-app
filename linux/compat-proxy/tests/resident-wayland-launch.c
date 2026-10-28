/* Keep a resident Wayland worker alive across a threaded payload exec. */
#define main payload_main
#include "adopt-fork-launch.c"
#undef main
#include <assert.h>
#include <sys/socket.h>
#include <wayland-server.h>
static struct wl_display *display;
static atomic_int server_ready;
static void *server(void *unused){
    (void)unused;
    /* Native-only adoption records must never be retired as payloads. */
    struct adopt_thread *native=reserve();assert(native);
    native->resident_fs=backend_fs_read();
    atomic_store(&native->tid,R(gettid,0,0,0,0,0,0));
    atomic_store(&server_ready,1);
    wl_display_run(display);
    return NULL;
}
int main(int argc,char **argv){
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    display=wl_display_create();assert(display);
    assert(wl_client_create(display,pair[0]));
    char fd[32];snprintf(fd,sizeof(fd),"%d",pair[1]);
    assert(!setenv("MATON_TEST_DISPLAY_FD",fd,1));
    pthread_t thread;assert(!pthread_create(&thread,NULL,server,NULL));
    while(!atomic_load(&server_ready))sched_yield();
    return payload_main(argc,argv);
}
