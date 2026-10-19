#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../wsi_wayland.c"

struct wl_proxy { const struct wl_interface *interface; uint32_t version; int dead; };
struct wl_display { int unused; };
struct wl_event_queue { int unused; };
static struct wl_proxy registry_proxy,dmabuf_proxy,params_proxy,buffer_proxy,sync_manager_proxy,sync_surface_proxy,ahb_manager_proxy,surface_proxy,callback_proxy;
static struct wl_display display;
static struct wl_event_queue queue;
static void (**registry_hooks)(void), (**buffer_hooks)(void), (**callback_hooks)(void);
static void *registry_data,*buffer_data,*callback_data;
static unsigned roundtrips,binds,param_creates,adds,immediates;
static int ahb_transport_fd=-1;
static const struct wl_interface exported_registry={"wl_registry",1,2,0,2,0};
static const struct wl_interface exported_surface={"wl_surface",4,11,0,0,0};
static const struct wl_interface exported_callback={"wl_callback",1,1,0,1,0};

static struct wl_proxy *fake_marshal(struct wl_proxy *p,uint32_t opcode,
        const struct wl_interface *iface,uint32_t version,uint32_t flags,...) {
    va_list ap;va_start(ap,flags);
    if (opcode==0 && flags==1) {
        assert(!iface && (p==&params_proxy || p==&buffer_proxy || p==&dmabuf_proxy || p==&sync_manager_proxy || p==&sync_surface_proxy || p==&ahb_manager_proxy || p==&callback_proxy));
        assert(!p->dead);p->dead=1;va_end(ap);return 0;
    }
    assert(!flags);
    if (p==(struct wl_proxy *)&display && opcode==1) {
        assert(iface==&exported_registry && version==1 && !va_arg(ap,void *));
        registry_proxy=(struct wl_proxy){iface,1,0};va_end(ap);return &registry_proxy;
    }
    if (p==&registry_proxy && opcode==0) {
        uint32_t name=va_arg(ap,uint32_t);const char *iname=va_arg(ap,const char *);
        uint32_t v=va_arg(ap,uint32_t);void *id=va_arg(ap,void *);
        assert(!id);
        if(name==77){assert(!strcmp(iname,"zwp_linux_dmabuf_v1") && v==4 && iface==&dmabuf_interface);binds++;dmabuf_proxy=(struct wl_proxy){iface,version,0};va_end(ap);return &dmabuf_proxy;}
        if(name==78){assert(!strcmp(iname,"maton_sync_file_v1")&&v==1&&iface==&sync_manager_interface);sync_manager_proxy=(struct wl_proxy){iface,version,0};va_end(ap);return &sync_manager_proxy;}
        assert(name==79&&!strcmp(iname,"maton_ahb_buffer_manager_v1")&&v==1&&iface==&ahb_manager_interface);
        ahb_manager_proxy=(struct wl_proxy){iface,version,0};va_end(ap);return &ahb_manager_proxy;
    }
    if(p==&sync_manager_proxy && opcode==1){void *id=va_arg(ap,void *);struct wl_proxy *surface=va_arg(ap,struct wl_proxy *);assert(!id&&surface==&surface_proxy&&iface==&sync_surface_interface);sync_surface_proxy=(struct wl_proxy){iface,version,0};va_end(ap);return &sync_surface_proxy;}
    if(p==&ahb_manager_proxy && opcode==1){void *id=va_arg(ap,void *);int fd=va_arg(ap,int);uint32_t w=va_arg(ap,uint32_t),h=va_arg(ap,uint32_t),format=va_arg(ap,uint32_t);assert(!id&&fd==ahb_transport_fd&&w==256&&h==256&&format==0x34324241u&&iface==&buffer_interface);char payload[8];assert(read(fd,payload,sizeof(payload))==(ssize_t)sizeof(payload)&&!memcmp(payload,"AHB-MOCK",8));buffer_proxy=(struct wl_proxy){iface,version,0};va_end(ap);return &buffer_proxy;}
    if(p==&sync_surface_proxy&&opcode==1){int fd=va_arg(ap,int);assert(fd==123&&!iface);va_end(ap);return 0;}
    if(p==&surface_proxy&&opcode==1){struct wl_proxy *buffer=va_arg(ap,struct wl_proxy *);int32_t x=va_arg(ap,int32_t),y=va_arg(ap,int32_t);assert(buffer==&buffer_proxy&&!x&&!y&&!iface);va_end(ap);return 0;}
    if(p==&surface_proxy&&opcode==3){void *id=va_arg(ap,void *);assert(!id&&iface==&exported_callback&&version==1);callback_proxy=(struct wl_proxy){iface,version,0};va_end(ap);return &callback_proxy;}
    if(p==&surface_proxy&&opcode==9){int32_t x=va_arg(ap,int32_t),y=va_arg(ap,int32_t),w=va_arg(ap,int32_t),h=va_arg(ap,int32_t);assert(!x&&!y&&!w&&!h&&!iface);va_end(ap);return 0;}
    if(p==&surface_proxy&&opcode==6){assert(!iface);va_end(ap);return 0;}
    if (p==&dmabuf_proxy && opcode==1) {
        void *id=va_arg(ap,void *);assert(!id && iface==&params_interface);
        param_creates++;params_proxy=(struct wl_proxy){iface,version,0};va_end(ap);return &params_proxy;
    }
    if (p==&params_proxy && opcode==1) {
        int fd=va_arg(ap,int);uint32_t plane=va_arg(ap,uint32_t),offset=va_arg(ap,uint32_t),stride=va_arg(ap,uint32_t),hi=va_arg(ap,uint32_t),lo=va_arg(ap,uint32_t);
        assert(fd==51 && plane==0 && offset==0 && stride==1024 && !hi && !lo && !iface);
        adds++;va_end(ap);return 0;
    }
    if (p==&params_proxy && opcode==3) {
        void *id=va_arg(ap,void *);int32_t w=va_arg(ap,int32_t),h=va_arg(ap,int32_t);
        uint32_t format=va_arg(ap,uint32_t),flags2=va_arg(ap,uint32_t);
        assert(!id && w==256 && h==256 && format==0x34324241 && !flags2 && iface==&buffer_interface && version==1);
        immediates++;buffer_proxy=(struct wl_proxy){iface,version,0};va_end(ap);return &buffer_proxy;
    }
    assert(!"unexpected Wayland marshal request");va_end(ap);return 0;
}
static uint32_t fake_version(struct wl_proxy *p){return p->version;}
static int fake_listener(struct wl_proxy *p,void (**hooks)(void),void *data){
    if(p==&registry_proxy){registry_hooks=hooks;registry_data=data;return 0;}
    if(p==&buffer_proxy){buffer_hooks=hooks;buffer_data=data;return 0;}
    if(p==&callback_proxy){callback_hooks=hooks;callback_data=data;return 0;}
    return -1;
}
static void fake_destroy(struct wl_proxy *p){p->dead=1;}
static struct wl_event_queue *fake_queue_create(struct wl_display *d){assert(d==&display);return &queue;}
static void fake_queue_destroy(struct wl_event_queue *q){assert(q==&queue);}
static int fake_roundtrip(struct wl_display *d,struct wl_event_queue *q){
    assert(d==&display && q==&queue && registry_hooks);roundtrips++;
    ((void (*)(void *,struct wl_proxy *,uint32_t,const char *,uint32_t))registry_hooks[0])(registry_data,&registry_proxy,77,"zwp_linux_dmabuf_v1",4);
    ((void (*)(void *,struct wl_proxy *,uint32_t,const char *,uint32_t))registry_hooks[0])(registry_data,&registry_proxy,78,"maton_sync_file_v1",1);
    ((void (*)(void *,struct wl_proxy *,uint32_t,const char *,uint32_t))registry_hooks[0])(registry_data,&registry_proxy,79,"maton_ahb_buffer_manager_v1",1);return 0;
}
static int fake_dispatch(struct wl_display *d,struct wl_event_queue *q){assert(d==&display && q==&queue);return 0;}
static int fake_dispatch_one(struct wl_display *d,struct wl_event_queue *q){assert(d==&display && q==&queue);if(callback_hooks)((void (*)(void *,struct wl_proxy *,uint32_t))callback_hooks[0])(callback_data,&callback_proxy,1);return 0;}
static struct wl_proxy *fake_wrapper(struct wl_proxy *p){return p;}
static void fake_wrapper_destroy(struct wl_proxy *p){assert(p==&dmabuf_proxy || p==(struct wl_proxy *)&display);}
static void fake_set_queue(struct wl_proxy *p,struct wl_event_queue *q){assert(p && q==&queue);}
static int fake_flush(struct wl_display *d){assert(d==&display);return 0;}
static int fake_dummy;
void *mv_wayland_symbol(const char *n){
    if(!strcmp(n,"wl_proxy_marshal_flags"))return fake_marshal;
    if(!strcmp(n,"wl_proxy_get_version"))return fake_version;
    if(!strcmp(n,"wl_proxy_add_listener"))return fake_listener;
    if(!strcmp(n,"wl_proxy_destroy"))return fake_destroy;
    if(!strcmp(n,"wl_display_create_queue"))return fake_queue_create;
    if(!strcmp(n,"wl_event_queue_destroy"))return fake_queue_destroy;
    if(!strcmp(n,"wl_display_roundtrip_queue"))return fake_roundtrip;
    if(!strcmp(n,"wl_display_dispatch_queue_pending"))return fake_dispatch;
    if(!strcmp(n,"wl_display_dispatch_queue"))return fake_dispatch_one;
    if(!strcmp(n,"wl_proxy_create_wrapper"))return fake_wrapper;
    if(!strcmp(n,"wl_proxy_wrapper_destroy"))return fake_wrapper_destroy;
    if(!strcmp(n,"wl_proxy_set_queue"))return fake_set_queue;
    if(!strcmp(n,"wl_display_flush"))return fake_flush;
    if(!strcmp(n,"wl_registry_interface"))return (void *)&exported_registry;
    if(!strcmp(n,"wl_surface_interface"))return (void *)&exported_surface;
    if(!strcmp(n,"wl_callback_interface"))return (void *)&exported_callback;
    (void)fake_dummy;return 0;
}
void *mv_alloc(size_t n){return __builtin_malloc(n);}
void mv_free(void *p){__builtin_free(p);}
int main(void){
    surface_proxy=(struct wl_proxy){&exported_surface,4,0};
    struct mv_wl_dmabuf *d=mv_wl_dmabuf_create(&display);assert(d && mv_wl_dmabuf_version(d)==4);
    assert(mv_wl_dmabuf_dispatch(d)==0);
    struct mv_wl_plane plane={.fd=51,.index=0,.offset=0,.stride=1024,.modifier=0};
    struct mv_wl_buffer *b=mv_wl_dmabuf_create_buffer(d,256,256,0x34324241,&plane,1);
    assert(b && !mv_wl_buffer_released(b));
    struct mv_wl_sync_surface *sync=mv_wl_sync_surface_create(d,(struct wl_surface *)&surface_proxy);assert(sync);
    assert(mv_wl_surface_present(sync,(struct wl_surface *)&surface_proxy,b,123)==0);
    assert(mv_wl_sync_wait_frame(sync)==0);
    assert(roundtrips==1 && binds==1 && param_creates==1 && adds==1 && immediates==1);
    ((void (*)(void *,struct wl_proxy *))buffer_hooks[0])(buffer_data,&buffer_proxy);
    assert(mv_wl_buffer_released(b));mv_wl_buffer_clear_released(b);assert(!mv_wl_buffer_released(b));
    mv_wl_buffer_destroy(b);
    assert(mv_wl_has_ahb_buffer(d));
    int pair[2];assert(socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair)==0);
    assert(write(pair[1],"AHB-MOCK",8)==8);ahb_transport_fd=pair[0];
    struct mv_wl_buffer *ahb=mv_wl_ahb_create_buffer(d,256,256,0x34324241,pair[0]);assert(ahb);
    close(pair[0]);close(pair[1]);
    ((void (*)(void *,struct wl_proxy *))buffer_hooks[0])(buffer_data,&buffer_proxy);
    assert(mv_wl_buffer_released(ahb));
    mv_wl_sync_surface_destroy(sync);mv_wl_buffer_destroy(ahb);mv_wl_dmabuf_destroy(d);assert(buffer_proxy.dead && dmabuf_proxy.dead && sync_manager_proxy.dead && sync_surface_proxy.dead && ahb_manager_proxy.dead && registry_proxy.dead);
    puts("PASS Wayland helper: private queue, dmabuf fallback and AHardwareBuffer transport wl_buffer");
}
