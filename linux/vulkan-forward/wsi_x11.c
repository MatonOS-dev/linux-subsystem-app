/* XCB 1.x / private AHB import / Present 1.0 ABI. The ICD has no DT_NEEDED:
 * resolve the application's Linux libraries, not the resident bionic ones.
 * AHB imports carry a transport socket; Present IdleNotify releases images.
 * No X events are taken from the application's ordinary event queue. */
#include "wsi_x11.h"
#include "runtime.h"
#include "backend.h"
#include "wayland_resolve.h"
/* Linux libc socket ABI; this freestanding ICD also builds for i386. */
struct mv_sockaddr { uint16_t family; char data[14]; };
struct mv_sockaddr_un { uint16_t sun_family; char sun_path[108]; };
struct mv_iovec { void *iov_base; size_t iov_len; };
struct mv_msghdr { void *msg_name; uint32_t msg_namelen; struct mv_iovec *msg_iov;
    size_t msg_iovlen; void *msg_control; size_t msg_controllen; int msg_flags; };
struct mv_cmsghdr { size_t cmsg_len; int cmsg_level,cmsg_type; };
#define MV_CMSG_ALIGN(n) (((n)+sizeof(size_t)-1)&~(sizeof(size_t)-1))
#define MV_CMSG_SPACE(n) (MV_CMSG_ALIGN(sizeof(struct mv_cmsghdr))+MV_CMSG_ALIGN(n))
#define MV_CMSG_LEN(n) (MV_CMSG_ALIGN(sizeof(struct mv_cmsghdr))+(n))
#define MV_CMSG_DATA(cm) ((unsigned char *)(cm)+MV_CMSG_ALIGN(sizeof(struct mv_cmsghdr)))

struct cookie { unsigned sequence; };
struct geometry { uint8_t type,depth; uint16_t sequence; uint32_t length,root;
    int16_t x,y; uint16_t width,height,border,pad; };
struct idle { uint8_t type,extension; uint16_t sequence; uint32_t length;
    uint16_t event_type,pad; uint32_t event,window,serial,pixmap,fence,full_sequence; };
struct iterator { void *data;int rem,index; };
struct visual { uint32_t id;uint8_t kind,bits;uint16_t entries;uint32_t red,green,blue,pad; };
struct attributes { uint8_t type,backing;uint16_t sequence;uint32_t length,visual; };
struct mv_x11 { void *connection,*queue; uint32_t window,event,stamp,serial,depth;
    struct mv_x11_buffer *buffers; uint64_t next_msc; int pending_complete,mailbox; };
struct mv_x11_buffer { struct mv_x11 *x; struct mv_x11_buffer *next;
    uint32_t pixmap,serial; int released; };
static void (*xfree)(void *);
static char *(*xgetenv)(const char *);
static int (*xopen)(const char *,int,...),(*xsnprintf)(char *,size_t,const char *,...);
static int (*xsock)(int,int,int),(*xconnect)(int,const struct mv_sockaddr *,uint32_t),(*xclose)(int);
static long (*xsendmsg)(int,const struct mv_msghdr *,int),(*xrecv)(int,void *,size_t,int);
static uint32_t (*generate)(void *);
static int (*flush)(void *),(*failed)(void *),(*get_fd)(void *);
static struct cookie (*get_geometry)(void *,uint32_t);
static struct geometry *(*geometry_reply)(void *,struct cookie,void **);
static void *(*check)(void *,struct cookie);
static struct cookie (*free_pixmap)(void *,uint32_t);
static struct cookie (*select_input)(void *,uint32_t,uint32_t,uint32_t);
static struct cookie (*present)(void *,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,int16_t,int16_t,
    uint32_t,uint32_t,uint32_t,uint32_t,uint64_t,uint64_t,uint64_t,uint32_t,const void *);
static void *(*register_queue)(void *,void *,uint32_t,uint32_t *);
static void (*unregister_queue)(void *,void *);
static struct idle *(*poll_event)(void *,void *),*(*wait_event)(void *,void *);
static void *present_id;
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
        void *xcb=mv_client_library("libxcb.so.1");
        void *pres=mv_client_library("libxcb-present.so.0"),*libc=mv_client_library("libc.so.6");
#define LOAD(v,l,n) v=(void *)mv_client_symbol(l,n)
        LOAD(xopen,libc,"open");LOAD(xsnprintf,libc,"snprintf");LOAD(xgetenv,libc,"getenv");LOAD(xfree,libc,"free");LOAD(xsock,libc,"socket");LOAD(xconnect,libc,"connect");
        LOAD(xclose,libc,"close");LOAD(xsendmsg,libc,"sendmsg");LOAD(xrecv,libc,"recv");
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
        LOAD(select_input,pres,"xcb_present_select_input_checked");
        LOAD(present,pres,"xcb_present_pixmap_checked");LOAD(present_id,pres,"xcb_present_id");
        LOAD(extension_data,xcb,"xcb_get_extension_data");
#undef LOAD
        initialized=1;
    }
    int ok=xopen&&xsnprintf&&xgetenv&&xfree&&xsock&&xconnect&&xclose&&xsendmsg&&xrecv&&generate&&flush&&failed&&get_fd&&get_geometry&&geometry_reply&&check&&free_pixmap&&
        register_queue&&unregister_queue&&poll_event&&wait_event&&select_input&&present&&present_id&&extension_data&&setup&&roots&&depths&&visuals&&next_screen&&next_depth&&next_visual&&get_attributes&&attributes_reply;
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
    /* The AHB backend supports the ordinary 24/32-bit X visuals. */
    int ok=(g->depth==24||g->depth==32)&&(!x->depth||x->depth==g->depth);
    if(ok)x->depth=g->depth;*width=g->width;*height=g->height;xfree(g);return ok;
}
int mv_x11_visual_supported(void *connection,uint32_t id) {
    if(!connection||!id||!resolve())return 0;
    const unsigned char *p=extension_data(connection,present_id);
    if(!p||!p[8]||!xgetenv("MATON_X11_AHB_SOCKET"))return 0;
    const unsigned char *s=setup(connection);
    /* X pixel operations require LSBFirst visuals. */
    if(!s||s[30]!=0)return 0;
    for(struct iterator a=roots(s);a.rem;next_screen(&a))
        for(struct iterator b=depths(a.data);b.rem;next_depth(&b)) {
            if(*(uint8_t *)b.data!=24&&*(uint8_t *)b.data!=32)continue;
            for(struct iterator c=visuals(b.data);c.rem;next_visual(&c)) {
                struct visual *v=c.data;
                /* TrueColor or DirectColor (GLX fbconfigs expose both); same pixel layout. */
                if(v->id==id)return (v->kind==4||v->kind==5)&&v->red==0xff0000&&v->green==0xff00&&v->blue==0xff;
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
void mv_x11_set_mailbox(struct mv_x11 *x,int mailbox) { x->mailbox=mailbox; }
void mv_x11_destroy(struct mv_x11 *x) {
    if(!x)return;
    (void)checked(x,select_input(x->connection,x->event,x->window,0));
    unregister_queue(x->connection,x->queue);mv_free(x);
}
/* One request per connection. The AHB is already queued on transport, so
 * the X server never waits for a producer while processing its event loop.
 * The client reserves the XID: normal X resource teardown owns the pixmap. */
struct mv_x11_buffer *mv_x11_buffer_create(struct mv_x11 *x,int transport,uint32_t width,uint32_t height,uint32_t format) {
    const char *path=xgetenv("MATON_X11_AHB_SOCKET");
    struct mv_sockaddr_un address={.sun_family=1};size_t n=0,slash=0;
    if(!path||transport<0||!width||!height)return 0;
    while(path[n]){if(path[n]=='/')slash=n;n++;}
    if(!slash||!path[slash+1])return 0;
    char *directory=mv_alloc(slash+1);if(!directory)return 0;
    memcpy(directory,path,slash);directory[slash]=0;
    int dir=xopen(directory,010000000|0200000|02000000); /* O_PATH|O_DIRECTORY|O_CLOEXEC */
    mv_free(directory);if(dir<0)return 0;
    int len=xsnprintf(address.sun_path,sizeof(address.sun_path),"/proc/self/fd/%d/%s",dir,path+slash+1);
    if(len<0||(size_t)len>=sizeof(address.sun_path)){xclose(dir);return 0;}
    int fd=xsock(1,5|02000000,0);if(fd<0){xclose(dir);return 0;}
    struct mv_x11_buffer *b=mv_alloc(sizeof(*b));if(!b){xclose(fd);xclose(dir);return 0;}
    memset(b,0,sizeof(*b));b->x=x;b->pixmap=generate(x->connection);b->released=1;
    uint32_t request[5]={width,height,format,x->window,b->pixmap},reply=0;
    struct mv_iovec iov={request,sizeof(request)};
    union {struct mv_cmsghdr align;unsigned char bytes[MV_CMSG_SPACE(sizeof(int))];} control={0};
    struct mv_msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    struct mv_cmsghdr *cm=((struct mv_cmsghdr *)msg.msg_control);cm->cmsg_level=1;cm->cmsg_type=1;cm->cmsg_len=MV_CMSG_LEN(sizeof(int));
    memcpy(MV_CMSG_DATA(cm),&transport,sizeof(transport));
    int ok=xconnect(fd,(const struct mv_sockaddr *)&address,sizeof(address))==0&&
        xsendmsg(fd,&msg,0x4000)==sizeof(request)&&xrecv(fd,&reply,sizeof(reply),0)==sizeof(reply)&&reply==b->pixmap;
    xclose(fd);xclose(dir);
    if(!ok){mv_free(b);return 0;}
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
    /* FIFO waits for the previous Complete and targets the next MSC. MAILBOX
     * targets MSC 0: Present replaces a still-queued pixmap (IdleNotify
     * releases it), so the newest frame shows at the next vblank. */
    while(!x->mailbox&&x->pending_complete)if(mv_x11_dispatch(x,-1)<0)return 0;
    if(x->next_msc==UINT64_MAX)return 0;
    b->serial=++x->serial;
    if(!checked(x,present(x->connection,x->window,b->pixmap,b->serial,0,0,0,0,0,0,0,0,x->mailbox?0:x->next_msc,0,0,0,0)))return 0;
    b->released=0;x->pending_complete=1;return flush(x->connection)>0;
}
