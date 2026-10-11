/* SPDX-License-Identifier: Apache-2.0
 * Host SPA regression: two clients, remove/unmap the first, keep playing.
 * No Android, native-protocol sockets, device, or per-app policy required.
 */
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <spa/support/plugin.h>
#include <spa/utils/names.h>
#include <spa/param/audio/format-utils.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/node/command.h>
#include <spa/buffer/alloc.h>
#include <spa/support/loop.h>

/* The test is single-threaded; execute the mixer's IO changes synchronously. */
static int locked(void *object, spa_invoke_func_t func, uint32_t seq,
        const void *data, size_t size, void *user) {
    return func(object, false, seq, data, size, user);
}
static const struct spa_loop_methods loop_methods = {
    SPA_VERSION_LOOP_METHODS, .locked=locked,
};

static const struct spa_pod *format(unsigned char storage[512]) {
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, 512);
    return spa_format_audio_dsp_build(&b, SPA_PARAM_Format,
            &SPA_AUDIO_INFO_DSP_INIT(.format = SPA_AUDIO_FORMAT_DSP_F32));
}
static struct spa_buffer **buffers(void *pcm) {
    struct spa_data d = { .type=SPA_DATA_MemPtr, .flags=SPA_DATA_FLAG_READWRITE,
        .maxsize=4096, .data=pcm };
    uint32_t align = 16;
    struct spa_buffer **b = spa_buffer_alloc_array(1, SPA_BUFFER_ALLOC_FLAG_NO_DATA,
            0, NULL, 1, &d, &align);
    assert(b);
    b[0]->datas[0].data = pcm;
    b[0]->datas[0].chunk->offset=0;
    b[0]->datas[0].chunk->size=256;
    b[0]->datas[0].chunk->stride=4;
    return b;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    spa_handle_factory_enum_func_t enumerate = dlsym(lib, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
    assert(enumerate);
    const struct spa_handle_factory *factory = NULL, *f;
    uint32_t index=0;
    while (enumerate(&f, &index) == 1)
        if (!strcmp(f->name, SPA_NAME_AUDIO_MIXER_DSP)) factory=f;
    assert(factory);
    struct spa_handle *handle=calloc(1,spa_handle_factory_get_size(factory,NULL));
    struct spa_loop loop = { .iface=SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_DataLoop,
            SPA_VERSION_LOOP, &loop_methods, &loop) };
    struct spa_support support = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DataLoop, &loop);
    assert(handle && spa_handle_factory_init(factory,handle,NULL,&support,1)==0);
    struct spa_node *node;
    assert(spa_handle_get_interface(handle,SPA_TYPE_INTERFACE_Node,(void**)&node)==0);
    unsigned char storage[512];
    const struct spa_pod *fmt=format(storage);
    struct spa_io_buffers io[3] = { SPA_IO_BUFFERS_INIT, SPA_IO_BUFFERS_INIT, SPA_IO_BUFFERS_INIT };
    void *pcm[3]; struct spa_buffer **buf[3];
    for (unsigned i=0;i<3;i++) {
        pcm[i]=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        assert(pcm[i]!=MAP_FAILED);
        buf[i]=buffers(pcm[i]);
        if (i<2) assert(spa_node_add_port(node,SPA_DIRECTION_INPUT,i,NULL)==0);
        enum spa_direction dir=i<2 ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT;
        uint32_t port=i<2 ? i : 0;
        assert(spa_node_port_set_param(node,dir,port,SPA_PARAM_Format,0,fmt)==0);
        assert(spa_node_port_use_buffers(node,dir,port,0,buf[i],1)==0);
        assert(spa_node_port_set_io(node,dir,port,SPA_IO_Buffers,&io[i],sizeof(io[i]))==0);
    }
    struct spa_io_position position = {0};
    position.clock.duration=64;
    assert(spa_node_set_io(node,SPA_IO_Position,&position,sizeof(position))==0);
    struct spa_command start=SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
    assert(spa_node_send_command(node,&start)==0);
    for (unsigned i=0;i<64;i++) { ((float*)pcm[0])[i]=0.125f; ((float*)pcm[1])[i]=0.25f; }
    io[0].status=io[1].status=SPA_STATUS_HAVE_DATA; io[0].buffer_id=io[1].buffer_id=0;
    assert(spa_node_process(node)&SPA_STATUS_HAVE_DATA);
    assert(buf[2][0]->datas[0].chunk->size==256);
    assert(((float*)pcm[2])[0]==0.375f);
    /* Match a departing peer: detach its IO and buffers before releasing memory. */
    struct spa_command pause=SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
    assert(spa_node_send_command(node,&pause)==0);
    assert(spa_node_port_set_io(node,SPA_DIRECTION_INPUT,0,SPA_IO_Buffers,NULL,0)==0);
    assert(spa_node_port_use_buffers(node,SPA_DIRECTION_INPUT,0,0,NULL,0)==0);
    assert(spa_node_remove_port(node,SPA_DIRECTION_INPUT,0)==0);
    free(buf[0]); assert(munmap(pcm[0],4096)==0);
    assert(spa_node_send_command(node,&start)==0);
    io[1].status=SPA_STATUS_HAVE_DATA; io[1].buffer_id=0;
    io[2].status=SPA_STATUS_NEED_DATA;
    assert(spa_node_process(node)&SPA_STATUS_HAVE_DATA);
    assert(((float*)pcm[2])[0]==0.25f);
    spa_handle_clear(handle); free(handle);
    for (unsigned i=1;i<3;i++) { free(buf[i]); munmap(pcm[i],4096); }
    dlclose(lib);
    puts("PASS: mixer owns output; surviving stream plays after peer unmap");
    return 0;
}
