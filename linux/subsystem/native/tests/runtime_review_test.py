#!/usr/bin/env python3
"""Host regression tests for runtime review fixes, using actual C functions.

Run: python3 linux/subsystem/native/tests/runtime_review_test.py
No Android guest or staged native libraries are modified.
"""
from pathlib import Path
import subprocess
import tempfile

NATIVE = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


core = (NATIVE / 'compositor_core.c').read_text()
audio = (NATIVE / 'pipewire_android.c').read_text()
common = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
'''
audio_test = common + r'''
#define PORTS 8
#define PW_KEY_LINK_OUTPUT_NODE "output-node"
#define PW_KEY_LINK_OUTPUT_PORT "output-port"
#define PW_KEY_LINK_INPUT_NODE "input-node"
#define PW_KEY_LINK_INPUT_PORT "input-port"
#define PW_TYPE_INTERFACE_Link "Link"
#define PW_VERSION_LINK 1
struct pw_proxy { unsigned destroyed; } proxies[16];
struct pw_properties { int dict; } properties;
''' + function(audio, 'struct port {') + ';' + r'''
static struct { struct port ports[PORTS]; uint32_t sink_node; void *core; } audio;
static unsigned created;
static struct pw_properties *pw_properties_new(void *a, void *b) { return &properties; }
static void pw_properties_setf(struct pw_properties *p, const char *k,
        const char *fmt, unsigned value) {}
static void pw_properties_free(struct pw_properties *p) {}
static struct pw_proxy *pw_core_create_object(void *c, const char *f,
        const char *t, unsigned v, void *p, unsigned s) { return &proxies[created++]; }
static void pw_proxy_destroy(struct pw_proxy *p) { assert(!p->destroyed++); }
'''
audio_test += function(audio, 'static void link_ports(')
audio_test += function(audio, 'static void global_remove(')
audio_test += r'''
int main(void) {
    audio.sink_node = 10;
    audio.ports[0] = (struct port){ .id=1, .node=20, .channel="FL", .output=true };
    audio.ports[1] = (struct port){ .id=2, .node=10, .channel="FL" };
    audio.ports[2] = (struct port){ .id=3, .node=20, .channel="FR", .output=true };
    audio.ports[3] = (struct port){ .id=4, .node=10, .channel="FR" };
    link_ports(); assert(created == 2);
    global_remove(NULL, 999); assert(created == 2 && !proxies[0].destroyed);
    global_remove(NULL, 2);
    assert(proxies[0].destroyed && !audio.ports[0].link && !audio.ports[0].input);
    assert(audio.ports[2].link == &proxies[1] && !proxies[1].destroyed);
    audio.ports[1] = (struct port){ .id=5, .node=10, .channel="FL" };
    link_ports(); assert(created == 3 && audio.ports[0].input == 5);
    global_remove(NULL, 1); assert(proxies[2].destroyed && !audio.ports[0].id);
    global_remove(NULL, 5); assert(!proxies[1].destroyed);
    puts("sink removal/recreation, output removal, unrelated links: PASS");
}
'''
core_test = common + r'''
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <poll.h>
struct wl_listener { int unused; };
struct wl_client { uid_t uid; bool destroyed; };
static void wl_client_get_credentials(struct wl_client *c, void *p, uid_t *u, void *g) { *u=c->uid; }
static void wl_client_destroy(struct wl_client *c) { c->destroyed=true; }
static struct {
    atomic_bool started, stopping;
    pthread_mutex_t mutex;
    pthread_cond_t ready_cond;
    pthread_t thread;
    int ready, ok, event_fd;
    char socket_name[108], runtime_dir[512];
} server = { .mutex=PTHREAD_MUTEX_INITIALIZER, .ready_cond=PTHREAD_COND_INITIALIZER,
             .event_fd=-1 };
static void *server_main(void *unused) {
    /* Finish successfully just after the caller's two-second deadline. */
    struct timespec delay = { .tv_sec=2, .tv_nsec=100000000 };
    nanosleep(&delay, NULL);
    server.event_fd=eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC);
    pthread_mutex_lock(&server.mutex);
    server.ready=server.ok=1;
    pthread_cond_broadcast(&server.ready_cond);
    pthread_mutex_unlock(&server.mutex);
    while (!atomic_load(&server.stopping)) {
        struct pollfd fd = { .fd=server.event_fd, .events=POLLIN };
        poll(&fd, 1, -1);
    }
    close(server.event_fd); server.event_fd=-1;
    return NULL;
}
'''
core_test += function(core, 'static void on_new_client(')
core_test += function(core, 'bool maton_core_start(')
core_test += r'''
int main(void) {
    struct wl_client owner = { .uid=getuid() }, foreign = { .uid=getuid()+1 };
    on_new_client(NULL, &owner); on_new_client(NULL, &foreign);
    assert(!owner.destroyed && foreign.destroyed);
    char directory[]="/tmp/maton-runtime-test-XXXXXX";
    assert(mkdtemp(directory)); assert(!chmod(directory, 0700));
    alarm(5); /* Regression would hang in pthread_join. */
    assert(!maton_core_start("wayland-0", directory));
    alarm(0);
    struct stat info; assert(!stat(directory, &info));
    assert((info.st_mode & 0777) == 0711);
    assert(!atomic_load(&server.started) && atomic_load(&server.stopping));
    rmdir(directory);
    puts("late startup shutdown, runtime mode, client UID gate: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='maton-runtime-review-') as output:
    for name, source in [('audio', audio_test), ('core', core_test)]:
        source_path = Path(output) / (name + '.c')
        binary = Path(output) / name
        source_path.write_text(source)
        subprocess.run(['cc', '-std=c11', '-pthread', str(source_path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=8)
