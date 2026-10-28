/* A threaded payload execs into a real Wayland client. */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
static void *idle(void *arg){(void)arg;for(;;)pause();return NULL;}
int main(int argc,char **argv){
    if(argc==1){
        pthread_t thread;assert(!pthread_create(&thread,NULL,idle,NULL));
        char *args[]={argv[0],"client",NULL};execv(argv[0],args);return 2;
    }
    assert(argc==2 && !strcmp(argv[1],"client"));
    struct wl_display *display=wl_display_connect_to_fd(atoi(getenv("MATON_TEST_DISPLAY_FD")));
    assert(display);assert(wl_display_roundtrip(display)>=0);
    wl_display_disconnect(display);
    return 0;
}
