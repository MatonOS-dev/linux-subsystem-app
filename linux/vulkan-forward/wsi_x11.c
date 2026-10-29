/* XCB 1.x / DRI3 1.0 / Present 1.0 ABI. The ICD has no DT_NEEDED:
 * resolve the application's Linux libraries, not the resident bionic ones.
 * DRI3 consumes a duplicated dma-buf fd; Present IdleNotify releases images.
 * No X events are taken from the application's ordinary event queue. */
#include "wsi_x11.h"
#include "runtime.h"
#include "backend.h"
#include "wayland_resolve.h"
struct cookie { unsigned sequence; };
struct geometry { uint8_t type,depth; uint16_t sequence; uint32_t length,root;
    int16_t x,y; uint16_t width,height,border,pad; };
struct idle { uint8_t type,extension; uint16_t sequence; uint32_t length;
    uint16_t event_type,pad; uint32_t event,window,serial,pixmap,fence,full_sequence; };
struct iterator { void *data;int rem,index; };
struct visual { uint32_t id;uint8_t kind,bits;uint16_t entries;uint32_t red,green,blue,pad; };
struct attributes { uint8_t type,backing;uint16_t sequence;uint32_t length,visual; };
struct mv_x11 { void *connection,*queue; uint32_t window,event,stamp,serial;
    struct mv_x11_buffer *buffers; uint64_t next_msc; int pending_complete; };
struct mv_x11_buffer { struct mv_x11 *x; struct mv_x11_buffer *next;
    uint32_t pixmap,serial; int released; };
static void (*xfree)(void *);
static int (*xdup)(int);
static uint32_t (*generate)(void *);
static int (*flush)(void *),(*failed)(void *),(*get_fd)(void *);
static struct cookie (*get_geometry)(void *,uint32_t);
static struct geometry *(*geometry_reply)(void *,struct cookie,void **);
static void *(*check)(void *,struct cookie);
static struct cookie (*pixmap_from_buffer)(void *,uint32_t,uint32_t,uint32_t,uint16_t,uint16_t,uint16_t,uint8_t,uint8_t,int32_t);
static struct cookie (*free_pixmap)(void *,uint32_t);
static struct cookie (*select_input)(void *,uint32_t,uint32_t,uint32_t);
static struct cookie (*present)(void *,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,int16_t,int16_t,
    uint32_t,uint32_t,uint32_t,uint32_t,uint64_t,uint64_t,uint64_t,uint32_t,const void *);
static void *(*register_queue)(void *,void *,uint32_t,uint32_t *);
static void (*unregister_queue)(void *,void *);
static struct idle *(*poll_event)(void *,void *),*(*wait_event)(void *,void *);
static void *present_id,*dri3_id;
static const unsigned char *(*extension_data)(void *,void *);
static const unsigned char *(*setup)(void *);
static struct iterator (*roots)(const void *),(*depths)(const void *),(*visuals)(const void *);
static void (*next_screen)(struct iterator *),(*next_depth)(struct iterator *),(*next_visual)(struct iterator *);
static struct cookie (*get_attributes)(void *,uint32_t);
static struct attributes *(*attributes_reply)(void *,struct cookie,void **);
static void *(*xlib_connection)(void *);
static int initialized,init_lock;
static int resolve(void) {
    while(__atomic_exchange_n(&init_lock,1,__ATOMIC_ACQUIRE)){}
    if(!initialized) {
        void *xcb=mv_client_library("libxcb.so.1"),*dri3=mv_client_library("libxcb-dri3.so.0");
        void *pres=mv_client_library("libxcb-present.so.0"),*libc=mv_client_library("libc.so.6");
#define LOAD(v,l,n) v=(void *)mv_client_symbol(l,n)
        LOAD(xfree,libc,"free");LOAD(xdup,libc,"dup");
        LOAD(generate,xcb,"xcb_generate_id");LOAD(flush,xcb,"xcb_flush");
        LOAD(failed,xcb,"xcb_connection_has_error");LOAD(get_fd,xcb,"xcb_get_file_descriptor");
        LOAD(get_geometry,xcb,"xcb_get_geometry");LOAD(geometry_reply,xcb,"xcb_get_geometry_reply");
        LOAD(setup,xcb,"xcb_get_setup");LOAD(roots,xcb,"xcb_setup_roots_iterator");
        LOAD(depths,xcb,"xcb_screen_allowed_depths_iterator");LOAD(visuals,xcb,"xcb_depth_visuals_iterator");
        LOAD(next_screen,xcb,"xcb_screen_next");LOAD(next_depth,xcb,"xcb_depth_next");LOAD(next_visual,xcb,"xcb_visualtype_next");
        LOAD(get_attributes,xcb,"xcb_get_window_attributes");LOAD(attributes_reply,xcb,"xcb_get_window_attributes_reply");
        LOAD(check,xcb,"xcb_request_check");LOAD(free_pixmap,xcb,"xcb_free_pixmap");
        LOAD(register_queue,xcb,"xcb_register_for_special_xge");
        LOAD(unregister_queue,xcb,"xcb_unregister_for_special_event");
        LOAD(poll_event,xcb,"xcb_poll_for_special_event");LOAD(wait_event,xcb,"xcb_wait_for_special_event");
        LOAD(pixmap_from_buffer,dri3,"xcb_dri3_pixmap_from_buffer_checked");
        LOAD(select_input,pres,"xcb_present_select_input_checked");
        LOAD(present,pres,"xcb_present_pixmap_checked");LOAD(present_id,pres,"xcb_present_id");
        LOAD(dri3_id,dri3,"xcb_dri3_id");LOAD(extension_data,xcb,"xcb_get_extension_data");
#undef LOAD
        initialized=1;
    }
    int ok=xfree&&xdup&&generate&&flush&&failed&&get_fd&&get_geometry&&geometry_reply&&check&&free_pixmap&&
        register_queue&&unregister_queue&&poll_event&&wait_event&&pixmap_from_buffer&&select_input&&present&&present_id&&dri3_id&&extension_data&&setup&&roots&&depths&&visuals&&next_screen&&next_depth&&next_visual&&get_attributes&&attributes_reply;
    __atomic_store_n(&init_lock,0,__ATOMIC_RELEASE);return ok;
}
void *mv_x11_connection(void *display) {
    if(!display)return 0;
    /* Xlib-XCB must already cooperate with Xlib's event ownership; only our
     * special Present queue is consumed by this ICD. */
    if(!xlib_connection)xlib_connection=mv_client_symbol(mv_client_library("libX11-xcb.so.1"),"XGetXCBConnection");
    return xlib_connection?xlib_connection(display):0;
}
static int checked(struct mv_x11 *x,struct cookie c) {
    void *error=check(x->connection,c);if(error){xfree(error);return 0;}return !failed(x->connection);
}
int mv_x11_extent(struct mv_x11 *x,uint32_t *width,uint32_t *height) {
    struct geometry *g=geometry_reply(x->connection,get_geometry(x->connection,x->window),0);
    if(!g)return 0;
    /* DRI3's 32-bit depth-24 pixmaps are B8G8R8X8. Do not claim support for
     * incompatible visuals (10-bit and indexed visuals need another format). */
    int ok=g->depth==24;*width=g->width;*height=g->height;xfree(g);return ok;
}
int mv_x11_visual_supported(void *connection,uint32_t id) {
    if(!connection||!id||!resolve())return 0;
    const unsigned char *d=extension_data(connection,dri3_id),*p=extension_data(connection,present_id);
    if(!d||!p||!d[8]||!p[8])return 0;
    const unsigned char *s=setup(connection);
    /* The dma-buf format is little-endian BGRX, matching X's LSBFirst. */
    if(!s||s[30]!=0)return 0;
    for(struct iterator a=roots(s);a.rem;next_screen(&a))
        for(struct iterator b=depths(a.data);b.rem;next_depth(&b)) {
            if(*(uint8_t *)b.data!=24)continue;
            for(struct iterator c=visuals(b.data);c.rem;next_visual(&c)) {
                struct visual *v=c.data;
                if(v->id==id)return v->kind==4&&v->red==0xff0000&&v->green==0xff00&&v->blue==0xff;
            }
        }
    return 0;
}
struct mv_x11 *mv_x11_create(void *connection,uint32_t window) {
    if(!connection||!window||!resolve())return 0;
    struct mv_x11 *x=mv_alloc(sizeof(*x));if(!x)return 0;memset(x,0,sizeof(*x));
    x->connection=connection;x->window=window;x->event=generate(connection);
    uint32_t w,h;
    if(!mv_x11_extent(x,&w,&h)){mv_free(x);return 0;}
    struct attributes *attrs=attributes_reply(connection,get_attributes(connection,window),0);
    int compatible=attrs&&mv_x11_visual_supported(connection,attrs->visual);
    if(attrs)xfree(attrs);
    if(!compatible){mv_free(x);return 0;}
    x->queue=register_queue(connection,present_id,x->event,&x->stamp);
    /* Completion serializes FIFO submissions; IdleNotify releases images. */
    if(!x->queue||!checked(x,select_input(connection,x->event,window,6))) {
        if(x->queue)unregister_queue(connection,x->queue);mv_free(x);return 0;
    }
    return x;
}
void mv_x11_destroy(struct mv_x11 *x) {
    if(!x)return;
    (void)checked(x,select_input(x->connection,x->event,x->window,0));
    unregister_queue(x->connection,x->queue);mv_free(x);
}
struct mv_x11_buffer *mv_x11_buffer_create(struct mv_x11 *x,int fd,uint32_t width,uint32_t height,uint32_t stride) {
    if(fd<0||!width||!height||width>65535||height>65535||stride>65535||stride<width*4||height>UINT32_MAX/stride)return 0;
    struct mv_x11_buffer *b=mv_alloc(sizeof(*b));if(!b)return 0;
    int owned=xdup(fd);if(owned<0){mv_free(b);return 0;}
    memset(b,0,sizeof(*b));b->x=x;b->pixmap=generate(x->connection);b->released=1;
    /* libxcb takes ownership of owned, including error paths. Original AHB
     * native-handle fds remain owned by Android until the image is destroyed. */
    if(!checked(x,pixmap_from_buffer(x->connection,b->pixmap,x->window,stride*height,
            width,height,stride,24,32,owned))){mv_free(b);return 0;}
    b->next=x->buffers;x->buffers=b;return b;
}
void mv_x11_buffer_destroy(struct mv_x11_buffer *b) {
    struct mv_x11 *x=b->x;struct mv_x11_buffer **p=&x->buffers;
    while(*p&&*p!=b)p=&(*p)->next;if(*p)*p=b->next;
    (void)free_pixmap(x->connection,b->pixmap);mv_free(b);
}
static int consume(struct mv_x11 *x,struct idle *e) {
    if(!e)return 0;
    if(e->event_type==2&&e->event==x->event&&e->window==x->window)
        for(struct mv_x11_buffer *b=x->buffers;b;b=b->next)
            if(b->pixmap==e->pixmap&&b->serial==e->serial)b->released=1;
    if(e->event_type==1&&e->event==x->event&&e->window==x->window&&e->serial==x->serial) {
        /* XCB adds full_sequence in the packed wire event, before msc. */
        uint64_t msc;memcpy(&msc,(unsigned char *)e+36,sizeof(msc));
        x->next_msc=msc==UINT64_MAX?msc:msc+1;x->pending_complete=0;
    }
    xfree(e);return 1;
}
int mv_x11_dispatch(struct mv_x11 *x,int timeout_ms) {
    int n=0;struct idle *e;
    while((e=poll_event(x->connection,x->queue)))n+=consume(x,e);
    if(n||!timeout_ms)return failed(x->connection)?-1:n;
    if(timeout_ms<0)return consume(x,wait_event(x->connection,x->queue))?1:-1;
    struct {int fd;short events,revents;} p={get_fd(x->connection),1,0};
    struct {long sec,nsec;} t={timeout_ms/1000,(timeout_ms%1000)*1000000L};
#if defined(__x86_64__)
    long r=mv_sys(271,(long)&p,1,(long)&t,0,0,0);
#elif defined(__i386__)
    long r=mv_sys(309,(long)&p,1,(long)&t,0,0,0);
#else
    long r=-38; (void)p;(void)t;
#endif
    if(r<=0)return (int)r;
    while((e=poll_event(x->connection,x->queue)))n+=consume(x,e);
    return failed(x->connection)?-1:(n?n:-4);
}
int mv_x11_released(struct mv_x11_buffer *b) {return b->released;}
int mv_x11_present(struct mv_x11_buffer *b,int fence) {
    if(fence>=0) {
        struct {int fd;short events,revents;} p={fence,1,0};long r;
        do {
#if defined(__x86_64__)
            r=mv_sys(271,(long)&p,1,0,0,0,0);
#elif defined(__i386__)
            r=mv_sys(309,(long)&p,1,0,0,0,0);
#else
            r=-38;
#endif
        }while(r==-4);
        if(r<0||!(p.revents&1))return 0;
    }
    struct mv_x11 *x=b->x;
    while(x->pending_complete)if(mv_x11_dispatch(x,-1)<0)return 0;
    if(x->next_msc==UINT64_MAX)return 0;
    b->serial=++x->serial;
    if(!checked(x,present(x->connection,x->window,b->pixmap,b->serial,0,0,0,0,0,0,0,0,x->next_msc,0,0,0,0)))return 0;
    b->released=0;x->pending_complete=1;return flush(x->connection)>0;
}
