/* In-process entry for the per-app session bus.
 *
 * OBSOLETE as a standalone service (see README): the broker is now loaded as a
 * JNI library by the app that owns the session, so the bus runs IN the app's
 * process at the app's UID. No executable is spawned and no privileged process
 * parses app bytes. This is the only supported entry going forward.
 *
 * The portal itself (JavaPortal) also runs in this process; the broker connects
 * to it over the existing MBP1-framed "portal-backend" LocalSocket, so the
 * protocol is unchanged.
 */
#include "broker.h"

#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Broker *g_broker;
static pthread_t g_thread;
static int g_running;

struct start_args {
    char *socket;
    char *policy;
    char *portal;
    char *secret;
};

/* MBP1 capability handshake to the in-process portal backend. */
static int connect_portal(const char *path, const char *secret) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (!path || strlen(path) >= sizeof(address.sun_path)) return -1;
    strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) { close(fd); return -1; }
    char accepted[4];
    if (!secret || strlen(secret) != 64 ||
            send(fd, "MBP1", 4, MSG_NOSIGNAL) != 4 ||
            send(fd, secret, 64, MSG_NOSIGNAL) != 64 ||
            recv(fd, accepted, sizeof(accepted), MSG_WAITALL) != 4 ||
            memcmp(accepted, "OKAY", 4)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void free_args(struct start_args *a) {
    if (!a) return;
    free(a->socket); free(a->policy); free(a->portal); free(a->secret); free(a);
}

static char *dup_string(JNIEnv *env, jstring value) {
    if (!value) return NULL;
    const char *chars = (*env)->GetStringUTFChars(env, value, NULL);
    char *copy = chars ? strdup(chars) : NULL;
    if (chars) (*env)->ReleaseStringUTFChars(env, value, chars);
    return copy;
}

/* Runs on the native thread: build the broker, wire the portal backend, then
 * block in the GLib main loop until broker_stop(). */
static void *run_broker(void *data) {
    struct start_args *a = data;
    GError *error = NULL;
    Broker *broker = broker_new(a->socket, a->policy, &error);
    int backend = broker ? connect_portal(a->portal, a->secret) : -1;
    /* JNI runs in the authenticated stub itself. Its UID is the session
     * identity; the old executable's environment is absent in an app process. */
    if (broker && backend >= 0 && broker_enable_host_session_for_uid(broker, geteuid(), &error) &&
            broker_register_session_services(broker, NULL, backend, &error)) {
        pthread_mutex_lock(&g_lock);
        g_broker = broker;
        g_running = 1;
        pthread_mutex_unlock(&g_lock);
        broker_run(broker, &error);           /* blocks; broker_stop() quits it */
    } else if (broker) {
        if (backend >= 0) close(backend);
        broker_free(broker);
    }
    if (error) g_warning("Per-app session broker: %s", error->message);
    g_clear_error(&error);
    pthread_mutex_lock(&g_lock);
    g_broker = NULL;
    g_running = 0;
    pthread_mutex_unlock(&g_lock);
    free_args(a);
    return NULL;
}

JNIEXPORT jboolean JNICALL
Java_org_matonos_compositor_runtime_NativeBroker_nativeStart(JNIEnv *env, jclass clazz,
        jstring socket, jstring policy, jstring portal, jstring secret) {
    (void)clazz;
    pthread_mutex_lock(&g_lock);
    int already = g_running;
    pthread_mutex_unlock(&g_lock);
    if (already) return JNI_FALSE;

    struct start_args *a = calloc(1, sizeof(*a));
    if (!a) return JNI_FALSE;
    a->socket = dup_string(env, socket);
    a->policy = dup_string(env, policy);
    a->portal = dup_string(env, portal);
    a->secret = dup_string(env, secret);
    if (!a->socket || !a->policy || !a->portal || !a->secret) {
        free_args(a);
        return JNI_FALSE;
    }
    if (pthread_create(&g_thread, NULL, run_broker, a)) {
        free_args(a);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_org_matonos_compositor_runtime_NativeBroker_nativeStop(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    pthread_mutex_lock(&g_lock);
    Broker *broker = g_broker;
    pthread_mutex_unlock(&g_lock);
    if (broker) broker_stop(broker);   /* g_main_loop_quit is thread-safe */
    pthread_join(g_thread, NULL);
}
