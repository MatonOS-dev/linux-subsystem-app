/* SPDX-License-Identifier: Apache-2.0
 * Per-stub PipeWire server. Its only hardware endpoint is Android AAudio.
 * All protocol parsing runs in the stub UID. No Linux payload gets Binder.
 */
#include <jni.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/port-config.h>
#include <spa/utils/names.h>
#include "short_socket_path.h"

#define TAG "MatonAudio"
#define LOG(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define FRAMES 8192u
#define PORTS 256u
struct port {
    uint32_t id, node, input;
    char channel[16];
    bool output;
    struct pw_proxy *link;
};
static struct {
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct pw_stream *sink;
    struct spa_hook registry_hook, sink_hook;
    struct port ports[PORTS];
    uint32_t sink_node;
    AAudioStream *android;
    atomic_uint read, write;
    atomic_bool disconnected;
    atomic_ulong nonzero_frames;
    bool reported_playback;
    int16_t samples[FRAMES * 2];
} audio;
static int runtime_dir_fd = -1;

/* Bounded single producer/single consumer queue: never block a PipeWire loop. */
static aaudio_data_callback_result_t android_data(AAudioStream *stream, void *user,
        void *buffer, int32_t frames) {
    (void)stream; (void)user;
    int16_t *output = buffer;
    unsigned read = atomic_load_explicit(&audio.read, memory_order_relaxed);
    unsigned write = atomic_load_explicit(&audio.write, memory_order_acquire);
    unsigned nonzero = 0;
    for (int32_t i = 0; i < frames; i++) {
        if (read != write) {
            output[2*i] = audio.samples[(read % FRAMES)*2];
            output[2*i+1] = audio.samples[(read % FRAMES)*2+1];
            if (output[2*i] || output[2*i+1]) nonzero++;
            read++;
        } else output[2*i] = output[2*i+1] = 0;
    }
    atomic_store_explicit(&audio.read, read, memory_order_release);
    atomic_fetch_add_explicit(&audio.nonzero_frames, nonzero, memory_order_relaxed);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void android_error(AAudioStream *stream, void *user, aaudio_result_t error) {
    (void)stream; (void)user; (void)error;
    atomic_store(&audio.disconnected, true);
}
static int open_android(void) {
    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return -1;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(builder, 48000);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(builder, android_data, NULL);
    AAudioStreamBuilder_setErrorCallback(builder, android_error, NULL);
    int result = AAudioStreamBuilder_openStream(builder, &audio.android);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) { LOG("AAudio open: %s", AAudio_convertResultToText(result)); return -1; }
    if (AAudioStream_getSampleRate(audio.android) != 48000 ||
            AAudioStream_getChannelCount(audio.android) != 2 ||
            AAudioStream_getFormat(audio.android) != AAUDIO_FORMAT_PCM_I16) {
        AAudioStream_close(audio.android); audio.android = NULL; return -1;
    }
    atomic_store(&audio.disconnected, false);
    return 0;
}
static void process(void *user) {
    (void)user;
    if (!audio.reported_playback && atomic_load(&audio.nonzero_frames)) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
                "Android consumed non-silent PipeWire PCM (UID %u)", (unsigned)getuid());
        audio.reported_playback = true;
    }
    struct pw_buffer *buffer = pw_stream_dequeue_buffer(audio.sink);
    if (!buffer) return;
    if (atomic_load(&audio.disconnected)) {
        // The callback only flags disconnection. Close/reopen on the PW main loop.
        if (audio.android) AAudioStream_close(audio.android);
        audio.android = NULL;
        if (open_android() == 0) AAudioStream_requestStart(audio.android);
    }
    struct spa_buffer *spa = buffer->buffer;
    if (audio.android && spa->n_datas > 0) {
        struct spa_data *data = &spa->datas[0];
        if (data->data && data->chunk && data->maxsize) {
            unsigned offset = data->chunk->offset % data->maxsize;
            unsigned size = SPA_MIN(data->chunk->size, data->maxsize - offset);
            unsigned read = atomic_load_explicit(&audio.read, memory_order_acquire);
            unsigned write = atomic_load_explicit(&audio.write, memory_order_relaxed);
            unsigned frames = SPA_MIN(size / 4, FRAMES - (write - read));
            const unsigned char *input = (unsigned char *)data->data + offset;
            for (unsigned i = 0; i < frames; i++)
                memcpy(&audio.samples[((write+i)%FRAMES)*2], input+i*4, 4);
            atomic_store_explicit(&audio.write, write+frames, memory_order_release);
        }
    }
    pw_stream_queue_buffer(audio.sink, buffer);
}
static void state_changed(void *user, enum pw_stream_state old,
        enum pw_stream_state state, const char *error) {
    (void)user;
    (void)old;
    if (state == PW_STREAM_STATE_ERROR) LOG("PipeWire sink: %s", error ? error : "unknown");
    if (!audio.android) return;
    if (state == PW_STREAM_STATE_STREAMING) AAudioStream_requestStart(audio.android);
    else if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_UNCONNECTED)
        AAudioStream_requestStop(audio.android);
}
static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS, .state_changed = state_changed, .process = process,
};

/* This tiny session policy has exactly one playback target. Configure audio
 * stream adapters as DSP ports and link their channels to the Android sink.
 * There is no capture source, ALSA enumeration or cross-app graph. */
static void link_ports(void) {
    for (unsigned i = 0; i < PORTS; i++) {
        struct port *out = &audio.ports[i];
        if (!out->id || !out->output || out->link || out->node == audio.sink_node) continue;
        for (unsigned j = 0; j < PORTS; j++) {
            struct port *in = &audio.ports[j];
            if (!in->id || in->output || in->node != audio.sink_node ||
                    strcmp(in->channel, out->channel)) continue;
            struct pw_properties *props = pw_properties_new(NULL, NULL);
            pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", out->node);
            pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", out->id);
            pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", in->node);
            pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", in->id);
            out->link = pw_core_create_object(audio.core, "link-factory",
                    PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0);
            pw_properties_free(props);
            if (out->link) out->input = in->id;
            break;
        }
    }
}
static void global(void *user, uint32_t id, uint32_t permissions,
        const char *type, uint32_t version, const struct spa_dict *props) {
    (void)user; (void)permissions; (void)version;
    if (!props) return;
    if (!strcmp(type, PW_TYPE_INTERFACE_Node)) {
        const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (name && !strcmp(name, "maton.android")) audio.sink_node = id;
        if (class && !strcmp(class, "Stream/Output/Audio")) {
            struct pw_node *node = pw_registry_bind(audio.registry, id,
                    PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
            if (node) {
                unsigned char storage[512];
                struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
                const struct spa_pod *param = spa_pod_builder_add_object(&builder,
                        SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
                        SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(SPA_DIRECTION_OUTPUT),
                        SPA_PARAM_PORT_CONFIG_mode, SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp));
                pw_node_set_param(node, SPA_PARAM_PortConfig, 0, param);
                pw_proxy_destroy((struct pw_proxy *)node);
            }
        }
    } else if (!strcmp(type, PW_TYPE_INTERFACE_Port)) {
        const char *node = spa_dict_lookup(props, PW_KEY_NODE_ID);
        const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        const char *channel = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL);
        if (!node || !direction || !channel) return;
        for (unsigned i = 0; i < PORTS; i++) if (!audio.ports[i].id) {
            audio.ports[i] = (struct port){ .id = id, .node = (uint32_t)strtoul(node,NULL,10),
                    .output = !strcmp(direction,"out") };
            snprintf(audio.ports[i].channel, sizeof(audio.ports[i].channel), "%s", channel);
            break;
        }
    }
    link_ports();
}
static void global_remove(void *user, uint32_t id) {
    (void)user;
    for (unsigned i = 0; i < PORTS; i++) {
        struct port *out = &audio.ports[i];
        if (out->link && out->input == id) {
            pw_proxy_destroy(out->link);
            out->link = NULL;
            out->input = 0;
        }
    }
    for (unsigned i = 0; i < PORTS; i++) if (audio.ports[i].id == id) {
        if (audio.ports[i].link) pw_proxy_destroy(audio.ports[i].link);
        memset(&audio.ports[i], 0, sizeof(audio.ports[i]));
    }
}
static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS, .global = global, .global_remove = global_remove,
};
static void stop(void) {
    if (audio.loop) pw_thread_loop_stop(audio.loop);
    if (audio.registry) {
        spa_hook_remove(&audio.registry_hook);
        for (unsigned i=0; i<PORTS; i++)
            if (audio.ports[i].link) pw_proxy_destroy(audio.ports[i].link);
        pw_proxy_destroy((struct pw_proxy *)audio.registry);
    }
    if (audio.sink) pw_stream_destroy(audio.sink);
    if (audio.core) pw_core_disconnect(audio.core);
    if (audio.context) pw_context_destroy(audio.context);
    if (audio.loop) pw_thread_loop_destroy(audio.loop);
    if (audio.android) AAudioStream_close(audio.android);
    memset(&audio, 0, sizeof(audio));
}
JNIEXPORT jboolean JNICALL Java_org_matonos_linux_runtime_NativeAudio_nativeStart(
        JNIEnv *env, jclass clazz, jstring runtime, jstring config) {
    (void)clazz;
    if (audio.loop) return JNI_FALSE;
    const char *run = (*env)->GetStringUTFChars(env, runtime, NULL);
    if (!run) return JNI_FALSE;
    const char *conf = (*env)->GetStringUTFChars(env, config, NULL);
    if (!conf) { (*env)->ReleaseStringUTFChars(env,runtime,run); return JNI_FALSE; }
    Dl_info location;
    char libraries[4096];
    bool success = false;
    if (!dladdr((void *)stop, &location) || !location.dli_fname) goto done;
    snprintf(libraries,sizeof(libraries),"%s",location.dli_fname);
    char *slash = strrchr(libraries,'/');
    if (!slash) goto done;
    *slash = 0; // Also works for APK!/lib/x86_64 paths (page-aligned JNI entries).
    runtime_dir_fd = maton_socket_dir_open(run);
    if (runtime_dir_fd < 0) goto done;
    char short_runtime[64];
    if (maton_socket_dir_path(short_runtime, sizeof(short_runtime), runtime_dir_fd) < 0) goto done;
    if (setenv("SPA_SUPPORT_LIB", "libspa-support", 1) ||
            setenv("PIPEWIRE_MODULE_DIR", libraries, 1) || setenv("SPA_PLUGIN_DIR",libraries,1) ||
            setenv("PIPEWIRE_RUNTIME_DIR",short_runtime,1) ||
            setenv("XDG_RUNTIME_DIR",short_runtime,1)) goto done;
    // Pulse expects PULSE_RUNTIME_PATH itself to contain native.
    char pulse[80]; snprintf(pulse,sizeof(pulse),"%s/pulse",short_runtime);
    if (setenv("PULSE_RUNTIME_PATH",pulse,1)) goto done;
    char logfile[4096]; snprintf(logfile,sizeof(logfile),"%s/pipewire.log",run);
    if (setenv("PIPEWIRE_LOG",logfile,1) || setenv("PIPEWIRE_LOG_COLOR","false",1)) goto done;
    pw_init(NULL,NULL);
    audio.sink_node = SPA_ID_INVALID;
    if (open_android()) goto fail;
    audio.loop = pw_thread_loop_new("maton-audio",NULL);
    if (!audio.loop) goto fail;
    struct pw_properties *props = pw_properties_new(
            PW_KEY_CONFIG_NAME,"maton.conf", PW_KEY_CONFIG_PREFIX,conf, NULL);
    audio.context = pw_context_new(pw_thread_loop_get_loop(audio.loop),props,0);
    if (!audio.context) goto fail;
    /* Never run a multi-stream sink with PipeWire's scheduler-only fallback.
     * It forwards peer buffer IDs rather than mixing into an owned buffer.
     * Missing APK plugins must disable audio, not leave dangling peer buffers
     * in the sink adapter after a playback stream disconnects. */
    struct spa_handle *mixer = pw_context_load_spa_handle(audio.context,
            SPA_NAME_AUDIO_MIXER_DSP, NULL);
    if (!mixer) { LOG("Required DSP audio mixer is unavailable"); goto fail; }
    pw_unload_spa_handle(mixer);
    // The exported pw_stream must have a native-protocol peer, including its
    // transport busy/pong state. Connect through our own private socket.
    audio.core = pw_context_connect(audio.context,
            pw_properties_new(PW_KEY_REMOTE_NAME,"pipewire-0",NULL),0);
    if (!audio.core) goto fail;
    audio.sink = pw_stream_new(audio.core,"Android playback",pw_properties_new(
            PW_KEY_NODE_NAME,"maton.android", PW_KEY_NODE_DESCRIPTION,"Android output",
            PW_KEY_MEDIA_CLASS,"Audio/Sink", PW_KEY_MEDIA_TYPE,"Audio",
            PW_KEY_NODE_GROUP,"maton.audio", "adapter.auto-port-config",
            "{ mode = dsp monitor = false position = preserve }", NULL));
    if (!audio.sink) goto fail;
    pw_stream_add_listener(audio.sink,&audio.sink_hook,&stream_events,NULL);
    unsigned char storage[512];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,sizeof(storage));
    const struct spa_pod *format = spa_format_audio_raw_build(&builder,SPA_PARAM_EnumFormat,
            &SPA_AUDIO_INFO_RAW_INIT(.format=SPA_AUDIO_FORMAT_S16,.rate=48000,.channels=2,
                    .position={SPA_AUDIO_CHANNEL_FL,SPA_AUDIO_CHANNEL_FR}));
    if (pw_stream_connect(audio.sink,PW_DIRECTION_INPUT,PW_ID_ANY,
            PW_STREAM_FLAG_MAP_BUFFERS, &format,1) < 0) goto fail;
    audio.registry = pw_core_get_registry(audio.core,PW_VERSION_REGISTRY,0);
    if (!audio.registry) goto fail;
    pw_registry_add_listener(audio.registry,&audio.registry_hook,&registry_events,NULL);
    if (pw_thread_loop_start(audio.loop) < 0) goto fail;
    success = true;
    goto done;
fail:
    LOG("Cannot start embedded PipeWire: %s",strerror(errno));
    stop();
done:
    if (!success && runtime_dir_fd >= 0) { close(runtime_dir_fd); runtime_dir_fd = -1; }
    (*env)->ReleaseStringUTFChars(env,config,conf);
    (*env)->ReleaseStringUTFChars(env,runtime,run);
    return success ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_org_matonos_linux_runtime_NativeAudio_nativeStop(
        JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    stop();
    if (runtime_dir_fd >= 0) { close(runtime_dir_fd); runtime_dir_fd = -1; }
}
