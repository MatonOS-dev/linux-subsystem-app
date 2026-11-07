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
    struct mv_x11_buffer *buffers; uint64_t next_msc; int pending_complete,mailbox; unsigned presents;
    uint32_t width,height; int suboptimal; };
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
static int *(*xerrno)(void);
/* Last mv_x11_buffer_create failure: stage*1000+errno (0 = none). */
int mv_x11_last_error;
void (*mv_x11_log_hook)(const char *,unsigned long);
static void note(const char *,long);
static int fail_at(int stage){mv_x11_last_error=stage*1000+(xerrno?*xerrno():0);note("maton-vulkan: X11 buffer create failed, stage*1000+errno",mv_x11_last_error);return 0;}
/* Gravestone: every close names its call site. */
static void close_at(int fd,int line){note("maton-vulkan: close at wsi_x11.c line",line);note("maton-vulkan: close fd",fd);if(xclose(fd)<0)note("maton-vulkan: close failed, errno",xerrno?*xerrno():0);}
#define MV_CLOSE(fd) close_at((fd),__LINE__)
static int resolve(void) {
    while(__atomic_exchange_n(&init_lock,1,__ATOMIC_ACQUIRE)){}
    if(!initialized) {
        void *xcb=mv_client_library("libxcb.so.1");
        void *pres=mv_client_library("libxcb-present.so.0"),*libc=mv_client_library("libc.so.6");
        /* XCB replies come from the program's malloc, which may be its own
         * (Chrome's PartitionAlloc); free must bind the same way: global scope. */
        void *global=mv_client_library(0);
#define LOAD(v,l,n) v=(void *)mv_client_symbol(l,n)
        LOAD(xopen,libc,"open");LOAD(xsnprintf,libc,"snprintf");LOAD(xgetenv,libc,"getenv");LOAD(xfree,global,"free");LOAD(xsock,libc,"socket");LOAD(xconnect,libc,"connect");
        LOAD(xclose,libc,"close");LOAD(xerrno,libc,"__errno_location");LOAD(xsendmsg,libc,"sendmsg");LOAD(xrecv,libc,"recv");
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
/* TEMPDIAG: chain in front of the app's Xlib IO error handler to log the
 * dying Display's libxcb error code and X client index, then call it. */
static int (*app_io_handler)(void *);
static int io_diag(void *display) {
    void *c=xlib_connection?xlib_connection(display):0;
    if(c&&failed&&setup){uint32_t base;memcpy(&base,setup(c)+12,4);note("maton-vulkan: Xlib IO error, xcb error code",failed(c));note("maton-vulkan: Xlib IO error, X client",(long)(base>>21));}
    else note("maton-vulkan: Xlib IO error, no xcb connection",0);
    return app_io_handler?app_io_handler(display):0;
}
static void io_hook(void) {
    static int io_hooked;
    if(!__atomic_exchange_n(&io_hooked,1,__ATOMIC_ACQ_REL)) {
        int (*(*set_io)(int (*)(void *)))(void *)=mv_client_symbol(mv_client_library("libX11.so.6"),"XSetIOErrorHandler");
        if(!xlib_connection)xlib_connection=mv_client_symbol(mv_client_library("libX11-xcb.so.1"),"XGetXCBConnection");
        if(set_io&&xlib_connection){app_io_handler=set_io(io_diag);note("maton-vulkan: Xlib IO handler chained",app_io_handler!=0);}
    }
}
void *mv_x11_connection(void *display) {
    if(!display)return 0;
    /* Xlib-XCB must already cooperate with Xlib's event ownership; only our
     * special Present queue is consumed by this ICD. */
    if(!xlib_connection)xlib_connection=mv_client_symbol(mv_client_library("libX11-xcb.so.1"),"XGetXCBConnection");
    return xlib_connection?xlib_connection(display):0;
}
/* TEMPDIAG: log libxcb's connection error code (1 socket, 2 ext, 3 mem,
 * 4 req len, 5 parse, 6 screen, 7 fd passing) at the first place we see it. */
static int xcb_err_seen;
static void xcb_err(struct mv_x11 *x,int where){
    int e=failed(x->connection);
    if(e&&!__atomic_exchange_n(&xcb_err_seen,1,__ATOMIC_ACQ_REL)){note("maton-vulkan: xcb connection error code",e);note("maton-vulkan: xcb error seen at wsi_x11.c line",where);note("maton-vulkan: xcb error window",(long)x->window);}
}
#define XERR(x) xcb_err((x),__LINE__)
static int checked(struct mv_x11 *x,struct cookie c) {
    void *error=check(x->connection,c);XERR(x);if(error){xfree(error);return 0;}return !failed(x->connection);
}
int mv_x11_extent(struct mv_x11 *x,uint32_t *width,uint32_t *height) {
    struct geometry *g=geometry_reply(x->connection,get_geometry(x->connection,x->window),0);
    if(!g){note("maton-vulkan: X11 GetGeometry failed, window",(long)x->window);return 0;}
    /* The AHB backend supports the ordinary 24/32-bit X visuals. */
    int ok=(g->depth==24||g->depth==32)&&(!x->depth||x->depth==g->depth);
    if(!ok){note("maton-vulkan: X11 window depth unsupported, window",(long)x->window);note("maton-vulkan: X11 window depth",(long)g->depth);}
    if(ok)x->depth=g->depth;*width=g->width;*height=g->height;xfree(g);return ok;
}
static int visual_check(void *connection,uint32_t id,int loud) {
    if(!connection||!id||!resolve())return 0;
    const unsigned char *p=extension_data(connection,present_id);
    if(!p||!p[8]){if(loud)note("maton-vulkan: X11 surface refused, Present extension missing, visual",(long)id);return 0;}
    if(!xgetenv("MATON_X11_AHB_SOCKET")){if(loud)note("maton-vulkan: X11 surface refused, MATON_X11_AHB_SOCKET unset, visual",(long)id);return 0;}
    const unsigned char *s=setup(connection);
    /* X pixel operations require LSBFirst visuals. */
    if(!s||s[30]!=0){if(loud)note("maton-vulkan: X11 surface refused, server not LSBFirst, visual",(long)id);return 0;}
    for(struct iterator a=roots(s);a.rem;next_screen(&a))
        for(struct iterator b=depths(a.data);b.rem;next_depth(&b)) {
            if(*(uint8_t *)b.data!=24&&*(uint8_t *)b.data!=32)continue;
            for(struct iterator c=visuals(b.data);c.rem;next_visual(&c)) {
                struct visual *v=c.data;
                /* TrueColor or DirectColor (GLX fbconfigs expose both); same pixel layout. */
                if(v->id!=id)continue;
                int ok=(v->kind==4||v->kind==5)&&v->red==0xff0000&&v->green==0xff00&&v->blue==0xff;
                if(!ok){if(loud){note("maton-vulkan: X11 surface refused, visual class/masks unsupported, visual",(long)id);note("maton-vulkan: X11 refused visual class",(long)v->kind);note("maton-vulkan: X11 refused visual red mask",(long)v->red);}}
                return ok;
            }
        }
    if(loud)note("maton-vulkan: X11 surface refused, visual not at depth 24/32, visual",(long)id);
    return 0;
}
int mv_x11_visual_supported(void *connection,uint32_t id){return visual_check(connection,id,0);}
struct mv_x11 *mv_x11_create(void *connection,uint32_t window) {
    if(!connection||!window){note("maton-vulkan: X11 surface refused, null connection/window, window",(long)window);return 0;}
    if(!resolve()){note("maton-vulkan: X11 surface refused, XCB symbols unresolved, window",(long)window);return 0;}
    io_hook();
    struct mv_x11 *x=mv_alloc(sizeof(*x));if(!x)return 0;memset(x,0,sizeof(*x));
    x->connection=connection;x->window=window;x->event=generate(connection);
    /* TEMPDIAG: X client index of this connection (resource_id_base>>21) and
     * the window's owner index, to match Xwayland's maton-close records. */
    {const unsigned char *s=setup(connection);uint32_t base;memcpy(&base,s+12,4);note("maton-vulkan: X11 surface create, connection client",(long)(base>>21));note("maton-vulkan: X11 surface create, window owner client",(long)(window>>21));note("maton-vulkan: X11 surface create, window",(long)window);}
    uint32_t w,h;
    if(!mv_x11_extent(x,&w,&h)){mv_free(x);return 0;}
    struct attributes *attrs=attributes_reply(connection,get_attributes(connection,window),0);
    if(!attrs)note("maton-vulkan: X11 GetWindowAttributes failed, window",(long)window);
    int compatible=attrs&&visual_check(connection,attrs->visual,1);
    if(attrs)xfree(attrs);
    if(!compatible){mv_free(x);return 0;}
    x->queue=register_queue(connection,present_id,x->event,&x->stamp);
    /* Configure reports window destruction; Completion serializes FIFO
     * submissions; IdleNotify releases images. */
    if(!x->queue||!checked(x,select_input(connection,x->event,window,7))) {
        note("maton-vulkan: X11 surface refused, Present event registration failed, window",(long)window);
        if(x->queue)unregister_queue(connection,x->queue);mv_free(x);return 0;
    }
    return x;
}
void mv_x11_set_mailbox(struct mv_x11 *x,int mailbox) { x->mailbox=mailbox; }
/* Swapchain extent; suboptimal until the window matches it again. */
void mv_x11_set_extent(struct mv_x11 *x,uint32_t width,uint32_t height) {
    uint32_t w,h;x->width=width;x->height=height;
    x->suboptimal=!mv_x11_extent(x,&w,&h)||w!=width||h!=height;
}
int mv_x11_suboptimal(struct mv_x11 *x) { return x->suboptimal; }
int mv_x11_has_alpha(struct mv_x11 *x) { return x->depth==32; }
void mv_x11_destroy(struct mv_x11 *x) {
    if(!x)return;
    note("maton-vulkan: X11 surface destroy, window",(long)x->window);XERR(x);
    (void)checked(x,select_input(x->connection,x->event,x->window,0));
    unregister_queue(x->connection,x->queue);mv_free(x);
}
/* One request per connection. The AHB is already queued on transport, so
 * the X server never waits for a producer while processing its event loop.
 * The client reserves the XID: normal X resource teardown owns the pixmap. */
/* Resolves Xwayland's AHB socket address; the caller connects, then closes
 * *dir. Returns an unconnected SEQPACKET socket or -1 (mv_x11_last_error). */
static int ahb_socket(struct mv_sockaddr_un *address,int *dir) {
    const char *path=xgetenv("MATON_X11_AHB_SOCKET");size_t n=0,slash=0;
    *dir=-1;
    if(!path){mv_x11_last_error=1000;return -1;}
    while(path[n]){if(path[n]=='/')slash=n;n++;}
    if(!slash||!path[slash+1]){mv_x11_last_error=2000;return -1;}
    char *directory=mv_alloc(slash+1);if(!directory){mv_x11_last_error=3000;return -1;}
    memcpy(directory,path,slash);directory[slash]=0;
    *dir=xopen(directory,010000000|0200000|02000000); /* O_PATH|O_DIRECTORY|O_CLOEXEC */
    mv_free(directory);
    if(*dir<0){fail_at(4);return -1;}
    /* Short paths connect directly (sandbox-mapped /tmp/.X11-unix); only
     * paths too long for sun_path go through the directory fd. */
    int len=n<sizeof(address->sun_path)?xsnprintf(address->sun_path,sizeof(address->sun_path),"%s",path):
        xsnprintf(address->sun_path,sizeof(address->sun_path),"/proc/self/fd/%d/%s",*dir,path+slash+1);
    if(len<0||(size_t)len>=sizeof(address->sun_path)){mv_x11_last_error=5000;MV_CLOSE(*dir);*dir=-1;return -1;}
    int fd=xsock(1,5|02000000,0);if(fd<0){fail_at(6);MV_CLOSE(*dir);*dir=-1;return -1;}
    return fd;
}
/* Stand-in requests on Xwayland's AHB socket: a 4-byte magic plus fds.
 * Returns the connected socket for the reply, or -1. */
static int ahb_request(uint32_t magic,const int *fds,int count) {
    struct mv_sockaddr_un address={.sun_family=1};int dir;
    mv_x11_last_error=0;
    if(!resolve()){mv_x11_last_error=13000;note("maton-vulkan: dma-buf AHB socket: libc/xcb symbols missing",0);return -1;}
    int fd=ahb_socket(&address,&dir);if(fd<0)return -1;
    struct mv_iovec iov={&magic,sizeof(magic)};
    union {struct mv_cmsghdr align;unsigned char bytes[MV_CMSG_SPACE(2*sizeof(int))];} control={0};
    struct mv_msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=MV_CMSG_SPACE(count*sizeof(int))};
    struct mv_cmsghdr *cm=((struct mv_cmsghdr *)msg.msg_control);cm->cmsg_level=1;cm->cmsg_type=1;cm->cmsg_len=MV_CMSG_LEN(count*sizeof(int));
    memcpy(MV_CMSG_DATA(cm),fds,count*sizeof(int));
    int rc;long sent=-1;
    do rc=xconnect(fd,(const struct mv_sockaddr *)&address,sizeof(address)); while(rc<0&&xerrno&&*xerrno()==4); /* EINTR */
    if(rc<0)fail_at(8);
    else {do sent=xsendmsg(fd,&msg,0x4000); while(sent<0&&xerrno&&*xerrno()==4);
        if(sent!=(long)sizeof(magic))fail_at(9);}
    MV_CLOSE(dir);
    if(sent!=(long)sizeof(magic)){MV_CLOSE(fd);return -1;}
    return fd;
}
/* Lookup {"MAHL"} + exported fd: Xwayland replies with the AHB
 * (AHardwareBuffer_sendHandleToUnixSocket) or hangs up. Caller receives on
 * the Android side and closes the socket. */
int mv_x11_ahb_lookup(int standin) { return ahb_request(0x4c48414du,&standin,1); }
/* Register {"MAHR"} + exported fd + AHB transport (already filled). Xwayland
 * acks with 4 bytes and keeps the registration until this socket closes. */
int mv_x11_ahb_register(int standin,int transport) {
    int fds[2]={standin,transport};
    int fd=ahb_request(0x5248414du,fds,2);if(fd<0)return -1;
    uint32_t ack=0;long got;
    do got=xrecv(fd,&ack,sizeof(ack),0); while(got<0&&xerrno&&*xerrno()==4);
    if(got!=(long)sizeof(ack)||ack!=1){fail_at(10);note("maton-vulkan: dma-buf register rejected by Xwayland, got",got);MV_CLOSE(fd);return -1;}
    return fd;
}
/* Registration sockets close through the gravestone. */
void mv_x11_ahb_unregister(int fd) { if(resolve())MV_CLOSE(fd); }
struct mv_x11_buffer *mv_x11_buffer_create(struct mv_x11 *x,int transport,uint32_t width,uint32_t height,uint32_t format) {
    struct mv_sockaddr_un address={.sun_family=1};int dir;
    mv_x11_last_error=0;
    if(transport<0||!width||!height){mv_x11_last_error=1000;return 0;}
    int fd=ahb_socket(&address,&dir);if(fd<0)return 0;
    struct mv_x11_buffer *b=mv_alloc(sizeof(*b));if(!b){mv_x11_last_error=7000;MV_CLOSE(fd);MV_CLOSE(dir);return 0;}
    memset(b,0,sizeof(*b));b->x=x;b->pixmap=generate(x->connection);b->released=1;
    if(b->pixmap==UINT32_MAX){note("maton-vulkan: X11 xcb_generate_id exhausted, window",(long)x->window);mv_x11_last_error=12000;mv_free(b);MV_CLOSE(fd);MV_CLOSE(dir);return 0;}
    uint32_t request[5]={width,height,format,x->window,b->pixmap},reply=0;
    struct mv_iovec iov={request,sizeof(request)};
    union {struct mv_cmsghdr align;unsigned char bytes[MV_CMSG_SPACE(sizeof(int))];} control={0};
    struct mv_msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    struct mv_cmsghdr *cm=((struct mv_cmsghdr *)msg.msg_control);cm->cmsg_level=1;cm->cmsg_type=1;cm->cmsg_len=MV_CMSG_LEN(sizeof(int));
    memcpy(MV_CMSG_DATA(cm),&transport,sizeof(transport));
    int ok=0,rc;
    do rc=xconnect(fd,(const struct mv_sockaddr *)&address,sizeof(address)); while(rc<0&&xerrno&&*xerrno()==4); /* EINTR */
    if(rc<0)fail_at(8);
    else {
        long sent;do sent=xsendmsg(fd,&msg,0x4000); while(sent<0&&xerrno&&*xerrno()==4);
        if(sent!=(long)sizeof(request))fail_at(9);
        else {
            long got;do got=xrecv(fd,&reply,sizeof(reply),0); while(got<0&&xerrno&&*xerrno()==4);
            if(got!=(long)sizeof(reply)) {
                fail_at(10);
                /* The request went out: Xwayland may have created the pixmap. A
                 * checked free settles it either way (BadPixmap = it did not). */
                (void)checked(x,free_pixmap(x->connection,b->pixmap));
            }
            else if(reply!=b->pixmap){mv_x11_last_error=11000;note("maton-vulkan: X11 AHB import rejected (see xwayland.log), pixmap",(long)b->pixmap);}
            else ok=1;
        }
    }
    MV_CLOSE(fd);MV_CLOSE(dir);
    if(!ok){mv_free(b);return 0;}
    b->next=x->buffers;x->buffers=b;return b;
}
void mv_x11_buffer_destroy(struct mv_x11_buffer *b) {
    struct mv_x11 *x=b->x;struct mv_x11_buffer **p=&x->buffers;
    while(*p&&*p!=b)p=&(*p)->next;if(*p)*p=b->next;
    (void)free_pixmap(x->connection,b->pixmap);XERR(x);mv_free(b);
}
static int consume(struct mv_x11 *x,struct idle *e) {
    if(!e)return 0;
    if(e->event_type==0&&e->window==x->window) {
        /* PresentConfigureNotify (XCB layout, full_sequence at 32): width/height
         * at 24/26, pixmap_flags at 40, bit 0 = PresentWindowDestroyed. */
        uint32_t flags;uint16_t w,h;memcpy(&flags,(unsigned char *)e+40,sizeof(flags));
        memcpy(&w,(unsigned char *)e+24,2);memcpy(&h,(unsigned char *)e+26,2);
        /* Like Mesa: a size change makes the swapchain suboptimal. */
        if(w!=x->width||h!=x->height)x->suboptimal=1;
        if(flags&1){note("maton-vulkan: X11 window destroyed, surface lost, window",(long)x->window);
            x->next_msc=UINT64_MAX;x->pending_complete=0;}
    }
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
    XERR(x);
    if(x->next_msc==UINT64_MAX)return -1;
    if(failed(x->connection)){note("maton-vulkan: X11 connection failed, window",(long)x->window);return -1;}
    if(n||!timeout_ms)return n;
    if(timeout_ms<0) {
        if(consume(x,wait_event(x->connection,x->queue)))return x->next_msc==UINT64_MAX?-1:1;
        note("maton-vulkan: X11 wait_event returned no event (connection dead), window",(long)x->window);return -1;
    }
    /* Another Xlib/XCB reader on this connection can pull our event into the
     * special-event queue while we sit in ppoll; the socket then stays quiet
     * and we would sleep the whole timeout. Poll in short slices and recheck
     * the queue between them. */
    for(;;) {
        struct {int fd;short events,revents;} p={get_fd(x->connection),1,0};
        int slice=timeout_ms<4?timeout_ms:4;
        struct {long sec,nsec;} t={0,slice*1000000L};
#if defined(__x86_64__)
        long r=mv_sys(271,(long)&p,1,(long)&t,0,0,0);
#elif defined(__i386__)
        long r=mv_sys(309,(long)&p,1,(long)&t,0,0,0);
#else
        long r=-38; (void)p;(void)t;
#endif
        while((e=poll_event(x->connection,x->queue)))n+=consume(x,e);
        if(x->next_msc==UINT64_MAX)return -1;
        if(failed(x->connection)){note("maton-vulkan: X11 connection failed, window",(long)x->window);return -1;}
        if(n)return n;
        if(r<0){note("maton-vulkan: X11 event ppoll failed",r);return (int)r;}
        if(r)return -4;
        timeout_ms-=slice;if(timeout_ms<=0)return 0;
    }
}
/* Present waits are unbounded by spec; say so in the log every 2 s instead of
 * hanging silently (Steam froze with no trace of where). */
static void note(const char *what,long value) {
    size_t n=0;while(what[n])n++;
#if defined(__i386__)
    mv_write("[i386] ",7);
#endif
    mv_write(what,n);
    char t[20];static const char hex[]="0123456789abcdef";t[0]=' ';t[1]='0';t[2]='x';
    for(unsigned j=0;j<16;j++)t[3+j]=hex[((uint64_t)value>>(60-4*j))&15];
    t[19]='\n';mv_write(t,20);
    if(mv_x11_log_hook)mv_x11_log_hook(what,(unsigned long)value);
}
int mv_x11_released(struct mv_x11_buffer *b) {return b->released;}
int mv_x11_present(struct mv_x11_buffer *b,int fence) {
    if(fence>=0) {
        struct {int fd;short events,revents;} p={fence,1,0};long r;unsigned waited=0;
        for(;;) {
            struct {long sec,nsec;} t={2,0};
#if defined(__x86_64__)
            r=mv_sys(271,(long)&p,1,(long)&t,0,0,0);
#elif defined(__i386__)
            r=mv_sys(309,(long)&p,1,(long)&t,0,0,0);
#else
            r=-38;(void)t;
#endif
            if(r==-4)continue;
            if(r)break;
            note("maton-vulkan: X11 present fence still unsignalled, seconds",waited+=2);
        }
        if(waited)note("maton-vulkan: X11 present fence done after seconds",waited);
        if(r<0||!(p.revents&1)){note("maton-vulkan: X11 present fence poll failed",r<0?r:p.revents);return 0;}
    }
    struct mv_x11 *x=b->x;
    /* FIFO waits for the previous Complete and targets the next MSC. MAILBOX
     * targets MSC 0: Present replaces a still-queued pixmap (IdleNotify
     * releases it), so the newest frame shows at the next vblank. */
    /* Unbounded by spec: block in XCB itself, which sees events another
     * reader already queued (a raw socket poll would not). */
    while(!x->mailbox&&x->pending_complete) {
        int r=mv_x11_dispatch(x,-1);
        if(r<0){note("maton-vulkan: X11 FIFO wait failed",r);return 0;}
    }
    if(x->next_msc==UINT64_MAX){note("maton-vulkan: X11 present after surface loss, window",(long)x->window);return 0;}
    b->serial=++x->serial;
    if(!checked(x,present(x->connection,x->window,b->pixmap,b->serial,0,0,0,0,0,0,0,0,x->mailbox?0:x->next_msc,0,0,0,0))){note("maton-vulkan: X11 PresentPixmap rejected, window",(long)x->window);return 0;}
    /* TEMPDIAG: Big Picture black window - do frames reach this window? */
    if(!x->presents++||!(x->presents%600))note("maton-vulkan: X11 presents, window",(long)x->window),note("maton-vulkan: X11 presents, count",(long)x->presents),note("maton-vulkan: X11 presents, pixmap",(long)b->pixmap),note("maton-vulkan: X11 presents, size",(long)x->width<<16|x->height),note("maton-vulkan: X11 presents, depth",(long)x->depth),note("maton-vulkan: X11 presents, mailbox msc",x->mailbox?-1L:(long)x->next_msc);
    b->released=0;x->pending_complete=1;int fl=flush(x->connection);XERR(x);return fl>0;
}
