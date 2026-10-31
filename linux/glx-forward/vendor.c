#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <glvnd/libglxabi.h>
#include "maton_egl_direct.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#define EXPORT __attribute__((visibility("default")))
#define BGRA 0x80E1
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define A(n) 0x8010
#define B(n) n
#define GLX_DRAWABLE_TYPE 0x8010
#define GLX_RENDER_TYPE 0x8011
#define GLX_X_RENDERABLE 0x8012
#define GLX_FBCONFIG_ID 0x8013
#define GLX_VISUAL_ID 0x800B
#define GLX_X_VISUAL_TYPE 0x22
#define GLX_TRUE_COLOR 0x8002
#define GLX_WINDOW_BIT 1
#define GLX_PIXMAP_BIT 2
#define GLX_PBUFFER_BIT 4
#define GLX_RGBA_BIT 1
#define GLX_RGBA 4
#define GLX_DOUBLEBUFFER 5
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_ALPHA_SIZE 11
#define GLX_DEPTH_SIZE 12
#define GLX_STENCIL_SIZE 13
#define GLX_BUFFER_SIZE 2
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_READ_BUFFER 0x0C02
#define GL_BACK 0x0405
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GLX_LEVEL 3
#define GLX_VENDOR 1
#define GLX_VERSION 2
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_FLAGS_ARB 0x2094
#define GLX_CONTEXT_DEBUG_BIT_ARB 1
#define GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 2
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 1
#define GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 2
#define GLX_CONTEXT_ES_PROFILE_BIT_EXT 4
#define GLX_WIDTH 0x801D
#define GLX_HEIGHT 0x801E
#define GLX_PBUFFER_HEIGHT 0x8040
#define GLX_PBUFFER_WIDTH 0x8041
#define GLX_PRESERVED_CONTENTS 0x801B
#define GLX_LARGEST_PBUFFER 0x801C
#define GLX_SWAP_INTERVAL_EXT 0x20F1
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_FLAGS 0x30FC
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD

typedef struct _XVisual { void *ext_data; unsigned long visualid; int c_class; unsigned long red_mask,green_mask,blue_mask; int bits_per_rgb,map_entries; } Visual;
typedef struct { int x,y; unsigned int width,height,border_width,depth; Visual *visual; unsigned long root; int c_class,bit_gravity,win_gravity,backing_store; unsigned long backing_planes,backing_pixel; int save_under; unsigned long colormap; int map_installed,map_state; long all_event_masks,your_event_mask,do_not_propagate_mask; int override_redirect; void *screen; } XWindowAttributes;
typedef struct { int width,height,xoffset,format; char *data; int byte_order,bitmap_unit,bitmap_bit_order,bitmap_pad,depth,bytes_per_line,bits_per_pixel; unsigned long red_mask,green_mask,blue_mask; char *obdata; struct { void *create_image,*destroy_image,*get_pixel,*put_pixel,*sub_image,*add_pixel; } f; } XImage;
extern int XScreenCount(Display*),XDefaultScreen(Display*),XDefaultDepth(Display*,int);
extern Visual *XDefaultVisual(Display*,int);
extern XVisualInfo *XGetVisualInfo(Display*,long,XVisualInfo*,int*);
extern int XFree(void*);
extern int XGetWindowAttributes(Display*,unsigned long,XWindowAttributes*),XFlush(Display*),XPutImage(Display*,unsigned long,void*,XImage*,int,int,int,int,unsigned int,unsigned int),XDestroyImage(XImage*),XFreeGC(Display*,void*);
extern void *XCreateGC(Display*,unsigned long,unsigned long,void*);
extern XImage *XCreateImage(Display*,Visual*,unsigned int,int,int,char*,unsigned int,unsigned int,int,int);
extern int XPutPixel(XImage*,int,int,unsigned long);
static EGLenum (*p_eglQueryAPI)(void); static EGLDisplay edpy=EGL_NO_DISPLAY; static EGLConfig ecfg; static __thread GLXContext current; static __thread Display *current_display;
static void *egl_lib; static const struct maton_egl_direct_v1 *egl_direct;
#define EGL_FN(name) static __typeof__(&name) p_##name
EGL_FN(eglGetPlatformDisplay); EGL_FN(eglGetDisplay); EGL_FN(eglInitialize);
EGL_FN(eglBindAPI); EGL_FN(eglChooseConfig); EGL_FN(eglCreateContext);
EGL_FN(eglCreatePbufferSurface); EGL_FN(eglMakeCurrent); EGL_FN(eglDestroySurface);
EGL_FN(eglDestroyContext); EGL_FN(eglGetProcAddress); EGL_FN(eglSwapInterval);
#undef EGL_FN
#define eglGetPlatformDisplay p_eglGetPlatformDisplay
#define eglGetDisplay p_eglGetDisplay
#define eglInitialize p_eglInitialize
#define eglBindAPI p_eglBindAPI
#define eglQueryAPI p_eglQueryAPI
#define eglChooseConfig p_eglChooseConfig
#define eglCreateContext p_eglCreateContext
#define eglCreatePbufferSurface p_eglCreatePbufferSurface
#define eglMakeCurrent p_eglMakeCurrent
#define eglDestroySurface p_eglDestroySurface
#define eglDestroyContext p_eglDestroyContext
#define eglGetProcAddress p_eglGetProcAddress
#define eglSwapInterval p_eglSwapInterval
/* GLX configs describe X visuals, even though they share a resident pbuffer
 * format. ANGLE selects its context by GLX_VISUAL_ID before creating windows. */
struct fb { Display *display; XVisualInfo visual; struct fb *next; };
static struct fb *configs;static unsigned configs_lock;
static void configs_acquire(void){while(__atomic_test_and_set(&configs_lock,__ATOMIC_ACQUIRE)){} }
static void configs_release(void){__atomic_clear(&configs_lock,__ATOMIC_RELEASE);}
static struct fb *find_config(Display *d,GLXFBConfig c){
    configs_acquire();struct fb *p=configs;while(p&&((GLXFBConfig)p!=c||p->display!=d))p=p->next;configs_release();return p;
}
static int init_configs(Display *d,int screen){
    configs_acquire();for(struct fb *c=configs;c;c=c->next)if(c->display==d&&c->visual.screen==screen){configs_release();return 1;}configs_release();
    XVisualInfo match={0};match.screen=screen;match.c_class=4;
    int count=0;XVisualInfo *visuals=XGetVisualInfo(d,2|8,&match,&count);if(!visuals)return 0;
    struct fb *head=0;
    for(int i=0;i<count;i++)if((visuals[i].depth==24||visuals[i].depth==32)&&visuals[i].red_mask==0xff0000&&visuals[i].green_mask==0xff00&&visuals[i].blue_mask==0xff){
        struct fb *c=malloc(sizeof(*c));if(!c){while(head){c=head;head=c->next;free(c);}XFree(visuals);return 0;}
        c->display=d;c->visual=visuals[i];c->next=head;head=c;
    }
    XFree(visuals);if(!head)return 0;
    configs_acquire();for(struct fb *c=configs;c;c=c->next)if(c->display==d&&c->visual.screen==screen){configs_release();while(head){struct fb *next=head->next;free(head);head=next;}return 1;}
    while(head){struct fb *next=head->next;head->next=configs;configs=head;head=next;}configs_release();return 1;
}
struct ctx { int magic,width,height,direct,swap_interval; EGLenum api; struct pbuffer *pbuffer; unsigned long long last_present_ns; EGLContext egl; EGLSurface surface; GLXDrawable drawable; struct ctx *next; };
/* Server-allocated XIDs keep pbuffer handles disjoint from window IDs. */
extern unsigned long XCreatePixmap(Display*,unsigned long,unsigned int,unsigned int,unsigned int);
extern unsigned long XRootWindow(Display*,int);
extern int XFreePixmap(Display*,unsigned long);
struct pbuffer { Display *display; GLXPbuffer id; int width,height; EGLSurface surface; unsigned refs; struct pbuffer *next; };
static struct pbuffer *pbuffers;
static struct ctx *contexts; static volatile int contexts_lock;
static void contexts_lock_acquire(void){while(__atomic_exchange_n(&contexts_lock,1,__ATOMIC_ACQUIRE)){} }
static void contexts_lock_release(void){__atomic_store_n(&contexts_lock,0,__ATOMIC_RELEASE);} static const char exts[]="GLX_ARB_create_context GLX_ARB_create_context_profile GLX_EXT_create_context_es_profile GLX_EXT_create_context_es2_profile GLX_EXT_swap_control GLX_MESA_swap_control GLX_SGI_swap_control GLX_EXT_visual_info";
/* The list owns one reference, and each context using a pbuffer owns one.
 * Destroying a current pbuffer defers its EGL destruction until it is detached. */
static struct pbuffer *find_pbuffer(Display *d,GLXDrawable id){
    contexts_lock_acquire();struct pbuffer *p=pbuffers;
    while(p&&(p->display!=d||p->id!=id))p=p->next;
    if(p)p->refs++;
    contexts_lock_release();return p;
}
static void put_pbuffer(struct pbuffer *p){
    if(!p)return;
    contexts_lock_acquire();unsigned refs=--p->refs;contexts_lock_release();
    if(!refs){eglDestroySurface(edpy,p->surface);free(p);}
}
extern long write(int,const void*,unsigned long);
static void egl_diag(const char*s){size_t n=0;while(s[n])n++;(void)write(2,s,n);}
#include "timing.h"
static unsigned long long monotonic_ns(void){struct timespec t;if(sys_clock_gettime(1,&t))return 0;return (unsigned long long)t.tv_sec*1000000000ull+(unsigned long long)t.tv_nsec;}
static void pace_present(struct ctx*x){if(!x||x->swap_interval<=0)return;unsigned long long now=monotonic_ns();if(!now)return;unsigned long long period=16666667ull*(unsigned)x->swap_interval;if(x->last_present_ns&&now-x->last_present_ns<period){unsigned long long left=period-(now-x->last_present_ns);struct timespec t={(long)(left/1000000000ull),(long)(left%1000000000ull)};(void)sys_nanosleep(&t);}x->last_present_ns=monotonic_ns();}
static void reset_egl_probe(void){
    egl_lib=0;egl_direct=0;
    p_eglGetPlatformDisplay=0;p_eglGetDisplay=0;p_eglInitialize=0;p_eglBindAPI=0;
    p_eglChooseConfig=0;p_eglCreateContext=0;p_eglCreatePbufferSurface=0;
    p_eglMakeCurrent=0;p_eglDestroySurface=0;p_eglDestroyContext=0;p_eglGetProcAddress=0;p_eglSwapInterval=0;
}
/* Loads the forwarder only; vendor probes must not start the Android driver
 * (its threads break single-threaded callers such as Chromium's zygote). */
static int load_egl(void){
    if(!egl_lib){
        egl_lib=dlopen("libEGL_maton.so",RTLD_NOW|RTLD_LOCAL);
        if(!egl_lib)egl_lib=dlopen("libEGL_maton.so.0",RTLD_NOW|RTLD_LOCAL);
        if(!egl_lib){egl_diag("maton GLX: cannot load direct Maton EGL vendor\n");return 0;}
        maton_egl_direct_v1_fn get_direct=(maton_egl_direct_v1_fn)dlsym(egl_lib,"maton_egl_direct_v1");
        if(!get_direct||(egl_direct=get_direct())==0||egl_direct->version!=1){egl_diag("maton GLX: direct Maton EGL ABI unavailable\n");reset_egl_probe();return 0;}
#define LOAD_DIRECT(name,field) p_##name=(__typeof__(p_##name))egl_direct->field
        LOAD_DIRECT(eglGetPlatformDisplay,get_platform_display);LOAD_DIRECT(eglGetDisplay,get_display);
        LOAD_DIRECT(eglInitialize,initialize);LOAD_DIRECT(eglBindAPI,bind_api);p_eglQueryAPI=egl_direct->query_api;
        LOAD_DIRECT(eglChooseConfig,choose_config);LOAD_DIRECT(eglCreateContext,create_context);
        LOAD_DIRECT(eglCreatePbufferSurface,create_pbuffer_surface);LOAD_DIRECT(eglMakeCurrent,make_current);
        LOAD_DIRECT(eglDestroySurface,destroy_surface);LOAD_DIRECT(eglDestroyContext,destroy_context);
        LOAD_DIRECT(eglGetProcAddress,get_proc_address);p_eglSwapInterval=(__typeof__(p_eglSwapInterval))egl_direct->get_proc_address("eglSwapInterval");
#undef LOAD_DIRECT
    }
    return 1;
}
static int init_egl(void){
    if(edpy!=EGL_NO_DISPLAY)return ecfg!=0;
    if(!load_egl())return 0;
    edpy=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if(edpy==EGL_NO_DISPLAY||!eglInitialize(edpy,0,0)){edpy=EGL_NO_DISPLAY;egl_diag("maton GLX: eglInitialize failed\n");return 0;}
    EGLenum previous_api=eglQueryAPI?eglQueryAPI():EGL_OPENGL_ES_API;
    if(!eglBindAPI(EGL_OPENGL_API)){egl_diag("maton GLX: eglBindAPI desktop GL failed\n");return 0;}
    /* GLX windows are rendered into this pbuffer. Keep the advertised GLX
     * depth/stencil sizes in sync with the actual default framebuffer. */
    EGLint a[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,24,EGL_STENCIL_SIZE,8,EGL_NONE};
    EGLint n=0;int chosen=eglChooseConfig(edpy,a,&ecfg,1,&n)&&n;eglBindAPI(previous_api);if(!chosen){egl_diag("maton GLX: no EGL desktop pbuffer config\n");return 0;}return 1;
}
static Bool supported(Display*d,int s){return d&&s>=0&&s<XScreenCount(d)&&load_egl();}
static void *dispatch(const GLubyte*n);
static int init_egl(void);
static void *getproc(const GLubyte*n){if(!n)return 0;void*p=dispatch(n);if(p)return p;if(!load_egl())return 0;return (void*)eglGetProcAddress((const char*)n);}
static int cfgattr(Display*d,GLXFBConfig c,int a,int*v){struct fb *f=find_config(d,c);if(!f||!v)return 1;switch(a){case GLX_FBCONFIG_ID:case GLX_VISUAL_ID:*v=(int)f->visual.visualid;break;case GLX_X_VISUAL_TYPE:*v=GLX_TRUE_COLOR;break;case GLX_X_RENDERABLE:*v=1;break;case GLX_DRAWABLE_TYPE:*v=GLX_WINDOW_BIT|GLX_PBUFFER_BIT;break;case GLX_RENDER_TYPE:*v=GLX_RGBA_BIT;break;case GLX_RED_SIZE:case GLX_GREEN_SIZE:case GLX_BLUE_SIZE:case GLX_ALPHA_SIZE:*v=8;break;case GLX_DEPTH_SIZE:*v=24;break;case GLX_STENCIL_SIZE:*v=8;break;case GLX_DOUBLEBUFFER:*v=1;break;default:*v=0;}return 0;}
static Bool glXQueryExtension(Display*d,int*e,int*v){if(e)*e=0;if(v)*v=0;return d&&load_egl();}
static Bool glXQueryVersion(Display*d,int*a,int*b){if(!d||!load_egl())return 0;if(a)*a=1;if(b)*b=4;return 1;}
static const char *glXGetClientString(Display*d,int n){(void)d;return n==GLX_VENDOR?"MatonOS":n==GLX_VERSION?"1.4":exts;}
static const char *glXQueryServerString(Display*d,int s,int n){return supported(d,s)?glXGetClientString(d,n):0;}
static const char *glXQueryExtensionsString(Display*d,int s){return supported(d,s)?exts:0;}
static int config_matches(Display *d,struct fb *c,const int *attrs){
    for(const int *p=attrs;p&&*p;p+=2){
        if(p[1]==-1)continue;
        int value=0;cfgattr(d,(GLXFBConfig)c,p[0],&value);
        switch(p[0]){
        case GLX_RED_SIZE:case GLX_GREEN_SIZE:case GLX_BLUE_SIZE:case GLX_ALPHA_SIZE:case GLX_DEPTH_SIZE:case GLX_STENCIL_SIZE:if(value<p[1])return 0;break;
        case GLX_DRAWABLE_TYPE:case GLX_RENDER_TYPE:if((value&p[1])!=p[1])return 0;break;
        case GLX_VISUAL_ID:case GLX_FBCONFIG_ID:case GLX_X_VISUAL_TYPE:case GLX_X_RENDERABLE:case GLX_DOUBLEBUFFER:if(value!=p[1])return 0;break;
        default:break; /* Preserve the existing limited attribute negotiation. */
        }
    }return 1;
}
static GLXFBConfig *glXChooseFBConfig(Display*d,int s,const int*a,int*n){
    if(n)*n=0;
    if(!n||!supported(d,s)||!init_configs(d,s))return 0;
    configs_acquire();struct fb *snapshot=configs;size_t count=0;for(struct fb *c=snapshot;c;c=c->next)if(c->display==d&&c->visual.screen==s)count++;configs_release();
    GLXFBConfig *result=malloc(count*sizeof(*result));if(!result)return 0;
    /* Config nodes are immutable and remain valid for the vendor lifetime. */
    size_t used=0;for(struct fb *c=snapshot;c;c=c->next)if(c->display==d&&c->visual.screen==s&&config_matches(d,c,a))result[used++]=(GLXFBConfig)c;
    *n=(int)used;if(!used){free(result);return 0;}return result;
}
static GLXFBConfig *glXGetFBConfigs(Display*d,int s,int*n){return glXChooseFBConfig(d,s,0,n);}
static int glXGetFBConfigAttrib(Display*d,GLXFBConfig c,int a,int*v){return cfgattr(d,c,a,v);}
static XVisualInfo *glXGetVisualFromFBConfig(Display*d,GLXFBConfig c){struct fb *f=find_config(d,c);if(!f)return 0;XVisualInfo *v=malloc(sizeof(*v));if(v)*v=f->visual;return v;}
static GLXFBConfig config_for_visual(Display *d,XVisualInfo *v){
    if(!d)return 0;
    int screen=v?v->screen:XDefaultScreen(d);if(!supported(d,screen)||!init_configs(d,screen))return 0;
    Visual *def=XDefaultVisual(d,screen);unsigned long id=v?v->visualid:def?def->visualid:0;
    configs_acquire();struct fb *c=configs;while(c&&(c->display!=d||c->visual.screen!=screen||c->visual.visualid!=id))c=c->next;configs_release();return (GLXFBConfig)c;
}
static XVisualInfo *glXChooseVisual(Display*d,int s,int*a){(void)a;if(!supported(d,s))return 0;Visual *v=XDefaultVisual(d,s);if(!v)return 0;XVisualInfo wanted={.screen=s,.visualid=v->visualid};return glXGetVisualFromFBConfig(d,config_for_visual(d,&wanted));}
static int glXGetConfig(Display*d,XVisualInfo*v,int a,int*out){if(!d||!v||!out)return 1;switch(a){case GLX_RGBA:case GLX_DOUBLEBUFFER:*out=1;break;case GLX_RED_SIZE:case GLX_GREEN_SIZE:case GLX_BLUE_SIZE:case GLX_ALPHA_SIZE:*out=8;break;case GLX_DEPTH_SIZE:*out=24;break;case GLX_STENCIL_SIZE:*out=8;break;case GLX_BUFFER_SIZE:*out=32;break;case GLX_LEVEL:*out=0;break;default:*out=0;}return 0;}
static GLXContext create_context(Display*d,GLXFBConfig c,GLXContext share,Bool direct,const int*a){struct fb *f=find_config(d,c);if(!f||!direct||!supported(d,f->visual.screen)||!init_egl())return 0;EGLContext share_egl=EGL_NO_CONTEXT;if(share){struct ctx *parent=(struct ctx *)share;if(parent->magic!=0x4d415443)return 0;share_egl=parent->egl;}struct ctx*x=calloc(1,sizeof(*x));if(!x)return 0;x->magic=0x4d415443;x->direct=direct;x->surface=EGL_NO_SURFACE;EGLint ea[16],n=0,major=0,minor=0,profile=0,flags=0;int seen_major=0,seen_minor=0,seen_profile=0,seen_flags=0;
    if(a)for(const int*p=a;*p!=0;p+=2){int v=p[1];switch(p[0]){case GLX_CONTEXT_MAJOR_VERSION_ARB:if(seen_major++||v<1)goto bad_attr;major=v;break;case GLX_CONTEXT_MINOR_VERSION_ARB:if(seen_minor++||v<0)goto bad_attr;minor=v;break;case GLX_CONTEXT_PROFILE_MASK_ARB:if(seen_profile++||(v!=GLX_CONTEXT_CORE_PROFILE_BIT_ARB&&v!=GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB&&v!=GLX_CONTEXT_ES_PROFILE_BIT_EXT))goto bad_attr;profile=v;break;case GLX_CONTEXT_FLAGS_ARB:if(seen_flags++||(v&~(GLX_CONTEXT_DEBUG_BIT_ARB|GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB)))goto bad_attr;flags=v;break;default:goto bad_attr;}}
    int es=profile==GLX_CONTEXT_ES_PROFILE_BIT_EXT;
    if(!es&&((major<3&&profile)||(major==3&&minor<2&&profile)||(major==3&&minor<2&&(flags&GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB))))goto bad_attr;
    if(es&&(flags&GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB))goto bad_attr;
    if(seen_major||seen_minor){ea[n++]=EGL_CONTEXT_MAJOR_VERSION;ea[n++]=seen_major?major:1;ea[n++]=EGL_CONTEXT_MINOR_VERSION;ea[n++]=seen_minor?minor:0;}
    if(profile&&!es){ea[n++]=EGL_CONTEXT_OPENGL_PROFILE_MASK;ea[n++]=profile==GLX_CONTEXT_CORE_PROFILE_BIT_ARB?1:2;}
    if(flags){ea[n++]=EGL_CONTEXT_FLAGS;ea[n++]=flags;}ea[n++]=EGL_NONE;
    EGLenum previous_api=eglQueryAPI?eglQueryAPI():EGL_OPENGL_ES_API;x->api=es?EGL_OPENGL_ES_API:EGL_OPENGL_API;if(!eglBindAPI(x->api)){free(x);return 0;}x->egl=eglCreateContext(edpy,ecfg,share_egl,ea);eglBindAPI(previous_api);if(x->egl==EGL_NO_CONTEXT){free(x);return 0;}contexts_lock_acquire();x->next=contexts;contexts=x;contexts_lock_release();return (GLXContext)x;bad_attr:free(x);return 0;}
static GLXContext glXCreateNewContext(Display*d,GLXFBConfig c,int r,GLXContext s,Bool direct){(void)r;return create_context(d,c,s,direct,0);}
static GLXContext glXCreateContext(Display*d,XVisualInfo*v,GLXContext s,Bool direct){return create_context(d,config_for_visual(d,v),s,direct,0);}
static GLXContext glXCreateContextAttribsARB(Display*d,GLXFBConfig c,GLXContext s,Bool direct,const int*a){return create_context(d,c,s,direct,a);}
static Bool glXMakeContextCurrent(Display*d,GLXDrawable draw,GLXDrawable read,GLXContext c){
    if(!d||read!=draw)return 0;
    if(!c){
        if(!eglMakeCurrent(edpy,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT))return 0;
        current=0;current_display=0;return 1;
    }
    struct ctx *x=(struct ctx *)c;XWindowAttributes w={0};
    struct pbuffer *pb=find_pbuffer(d,draw);
    if(pb){w.width=pb->width;w.height=pb->height;}
    else if(!draw||!XGetWindowAttributes(d,draw,&w)||!w.width||!w.height)return 0;
    EGLenum previous_api=eglQueryAPI?eglQueryAPI():EGL_OPENGL_ES_API;
    if(!eglBindAPI(x->api)){put_pbuffer(pb);return 0;}
    if(x->surface!=EGL_NO_SURFACE&&x->pbuffer==pb&&x->drawable==draw&&x->width==(int)w.width&&x->height==(int)w.height){
        Bool ok=eglMakeCurrent(edpy,x->surface,x->surface,x->egl);
        eglBindAPI(previous_api);put_pbuffer(pb);
        if(ok){current=c;current_display=d;}return ok;
    }
    EGLint attrs[]={EGL_WIDTH,(EGLint)w.width,EGL_HEIGHT,(EGLint)w.height,EGL_NONE};
    EGLSurface replacement=pb?pb->surface:eglCreatePbufferSurface(edpy,ecfg,attrs);
    if(replacement==EGL_NO_SURFACE){eglBindAPI(previous_api);put_pbuffer(pb);return 0;}
    if(!eglMakeCurrent(edpy,replacement,replacement,x->egl)){
        if(!pb)eglDestroySurface(edpy,replacement);
        eglBindAPI(previous_api);put_pbuffer(pb);return 0;
    }
    eglBindAPI(previous_api);
    if(x->pbuffer)put_pbuffer(x->pbuffer);
    else if(x->surface!=EGL_NO_SURFACE)eglDestroySurface(edpy,x->surface);
    x->pbuffer=pb;x->surface=replacement;x->width=w.width;x->height=w.height;
    x->drawable=draw;current=c;current_display=d;return 1;
}
static Bool glXMakeCurrent(Display*d,GLXDrawable w,GLXContext c){return glXMakeContextCurrent(d,w,w,c);}
static GLXContext glXGetCurrentContext(void){return (GLXContext)current;}
static Display *glXGetCurrentDisplay(void){return current_display;}
static GLXDrawable glXGetCurrentDrawable(void){return current?((struct ctx*)current)->drawable:0;}
static GLXDrawable glXGetCurrentReadDrawable(void){return glXGetCurrentDrawable();}
static void glXSwapBuffers(Display*d,GLXDrawable w){
    if(!d||!w||!current)return;
    if(((struct ctx*)current)->pbuffer)return;
    struct ctx*x=(struct ctx*)current;XWindowAttributes a;
    if(!XGetWindowAttributes(d,w,&a)||!a.width||!a.height)return;
    pace_present(x);
    if(x->width!=(int)a.width||x->height!=(int)a.height){
        eglMakeCurrent(edpy,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        eglDestroySurface(edpy,x->surface);
        EGLint q[]={EGL_WIDTH,(EGLint)a.width,EGL_HEIGHT,(EGLint)a.height,EGL_NONE};
        x->surface=eglCreatePbufferSurface(edpy,ecfg,q);
        if(x->surface==EGL_NO_SURFACE||!eglMakeCurrent(edpy,x->surface,x->surface,x->egl))return;
        x->width=a.width;x->height=a.height;
    }
    size_t z=(size_t)a.width*a.height*4;char*p=malloc(z);if(!p)return;
    /* GLVND has no current EGL dispatch here: GLX owns the dispatch slot.
     * Use the same TLS-aware Maton thunks supplied to client GL calls. */
    void (*finish)(void)=(void (*)(void))eglGetProcAddress("glFinish");
    void (*pack)(GLenum,GLint)=(void (*)(GLenum,GLint))eglGetProcAddress("glPixelStorei");
    void (*get)(GLenum,GLint*)=(void (*)(GLenum,GLint*))eglGetProcAddress("glGetIntegerv");
    void (*bind_framebuffer)(GLenum,GLuint)=(void (*)(GLenum,GLuint))eglGetProcAddress("glBindFramebuffer");
    void (*bind_buffer)(GLenum,GLuint)=(void (*)(GLenum,GLuint))eglGetProcAddress("glBindBuffer");
    void (*read_buffer)(GLenum)=(void (*)(GLenum))eglGetProcAddress("glReadBuffer");
    void (*read_pixels)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*)=
        (void (*)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*))eglGetProcAddress("glReadPixels");
    if(!finish||!pack||!get||!bind_framebuffer||!bind_buffer||!read_buffer||!read_pixels){free(p);return;}
    GLint old_read=0,old_pack_buffer=0,old_alignment=4,default_read=GL_BACK;
    finish();get(GL_READ_FRAMEBUFFER_BINDING,&old_read);get(GL_PIXEL_PACK_BUFFER_BINDING,&old_pack_buffer);
    get(GL_PACK_ALIGNMENT,&old_alignment);bind_framebuffer(GL_READ_FRAMEBUFFER,0);get(GL_READ_BUFFER,&default_read);
    bind_buffer(GL_PIXEL_PACK_BUFFER,0);read_buffer(GL_BACK);pack(GL_PACK_ALIGNMENT,1);
    read_pixels(0,0,a.width,a.height,BGRA,GL_UNSIGNED_BYTE,p);
    pack(GL_PACK_ALIGNMENT,old_alignment);read_buffer((GLenum)default_read);
    bind_buffer(GL_PIXEL_PACK_BUFFER,(GLuint)old_pack_buffer);bind_framebuffer(GL_READ_FRAMEBUFFER,(GLuint)old_read);
    XImage*i=XCreateImage(d,a.visual,a.depth,2,0,0,a.width,a.height,32,0);
    if(i){
        i->data=calloc((size_t)i->bytes_per_line,(size_t)i->height);
        if(!i->data){XDestroyImage(i);free(p);return;}
        unsigned long rm=a.visual->red_mask,gm=a.visual->green_mask,bm=a.visual->blue_mask;
        unsigned rs=0,gs=0,bs=0;while(rs<sizeof(unsigned long)*8&&!(rm&(1ul<<rs)))rs++;
        while(gs<sizeof(unsigned long)*8&&!(gm&(1ul<<gs)))gs++;
        while(bs<sizeof(unsigned long)*8&&!(bm&(1ul<<bs)))bs++;
        unsigned long rmax=rm>>rs,gmax=gm>>gs,bmax=bm>>bs;
        for(unsigned y=0;y<a.height;y++)for(unsigned x=0;x<a.width;x++){
            size_t k=((size_t)(a.height-1-y)*a.width+x)*4;
            unsigned long r=((unsigned long)(unsigned char)p[k+2]*rmax+127)/255;
            unsigned long g=((unsigned long)(unsigned char)p[k+1]*gmax+127)/255;
            unsigned long b=((unsigned long)(unsigned char)p[k]*bmax+127)/255;
            XPutPixel(i,(int)x,(int)y,((r<<rs)&rm)|((g<<gs)&gm)|((b<<bs)&bm));
        }
        free(p);
        void*g=XCreateGC(d,w,0,0);
        if(g){XPutImage(d,w,g,i,0,0,0,0,a.width,a.height);XFreeGC(d,g);}
        XDestroyImage(i);
    }else free(p);
    XFlush(d);
}
static void glXDestroyContext(Display*d,GLXContext c){(void)d;struct ctx*x=(struct ctx*)c;if(!x||x->magic!=0x4d415443)return;if(current==(GLXContext)x){eglMakeCurrent(edpy,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);current=0;}if(x->pbuffer)put_pbuffer(x->pbuffer);else if(x->surface!=EGL_NO_SURFACE)eglDestroySurface(edpy,x->surface);eglDestroyContext(edpy,x->egl);contexts_lock_acquire();struct ctx**p=&contexts;while(*p&&*p!=x)p=&(*p)->next;if(*p)*p=x->next;x->magic=0;contexts_lock_release();free(x);}
static Bool glXIsDirect(Display*d,GLXContext c){(void)d;struct ctx*x=(struct ctx *)c;return x&&x->magic==0x4d415443&&x->direct;}
static void glXWaitGL(void){if(current){void (*finish)(void)=(void (*)(void))eglGetProcAddress("glFinish");if(finish)finish();}} static void glXWaitX(void){}
static void glXQueryDrawable(Display*d,GLXDrawable w,int a,unsigned int*v){if(!v)return;*v=0;struct pbuffer *pb=find_pbuffer(d,w);if(pb){if(a==GLX_WIDTH)*v=pb->width;if(a==GLX_HEIGHT)*v=pb->height;put_pbuffer(pb);return;}if(!d||!current)return;struct ctx*x=(struct ctx*)current;if(x->drawable!=w)return;XWindowAttributes wa;if(a==GLX_SWAP_INTERVAL_EXT){*v=(unsigned)x->swap_interval;return;}if((a==GLX_WIDTH||a==GLX_HEIGHT)&&XGetWindowAttributes(d,w,&wa))*v=a==GLX_WIDTH?wa.width:wa.height;}
static void glXSwapIntervalEXT(Display*d,GLXDrawable w,int i){(void)d;if(!current||i<0)return;struct ctx*x=(struct ctx*)current;if(x->drawable!=w)return;if(eglSwapInterval&&eglSwapInterval(edpy,i))x->swap_interval=i;}
static int glXSwapIntervalMESA(unsigned int i){if(!current)return 1;if(i>2147483647u)return 2;glXSwapIntervalEXT(current_display,((struct ctx*)current)->drawable,(int)i);return ((struct ctx*)current)->swap_interval==(int)i?0:2;}
static int glXGetSwapIntervalMESA(void){return current?((struct ctx*)current)->swap_interval:0;}
static int glXSwapIntervalSGI(int i){if(i<0)return 2;if(!current)return 1;glXSwapIntervalEXT(current_display,((struct ctx*)current)->drawable,i);return ((struct ctx*)current)->swap_interval==i?0:2;}
static __GLXextFuncPtr glXGetProcAddressARB(const GLubyte*n){return (__GLXextFuncPtr)getproc(n);} static __GLXextFuncPtr glXGetProcAddress(const GLubyte*n){return glXGetProcAddressARB(n);}
static void glXCopyContext(Display*d,GLXContext a,GLXContext b,unsigned long m){(void)d;(void)a;(void)b;(void)m;}
static GLXPixmap glXCreateGLXPixmap(Display*d,XVisualInfo*v,unsigned long p){(void)d;(void)v;(void)p;return 0;}
static void glXDestroyGLXPixmap(Display*d,GLXPixmap p){(void)d;(void)p;}
static void glXUseXFont(unsigned long f,int first,int count,int list){(void)f;(void)first;(void)count;(void)list;}
static GLXWindow glXCreateWindow(Display*d,GLXFBConfig c,unsigned long w,const int*a){(void)a;struct fb *f=find_config(d,c);XWindowAttributes wa;return f&&XGetWindowAttributes(d,w,&wa)&&wa.visual&&wa.visual->visualid==f->visual.visualid?(GLXWindow)w:0;}
static void glXDestroyWindow(Display*d,GLXWindow w){(void)d;(void)w;}
static GLXPixmap glXCreatePixmap(Display*d,GLXFBConfig c,unsigned long p,const int*a){(void)d;(void)c;(void)p;(void)a;return 0;}
static void glXDestroyPixmap(Display*d,GLXPixmap p){(void)d;(void)p;}
static GLXPbuffer glXCreatePbuffer(Display*d,GLXFBConfig c,const int*a){
    struct fb *f=find_config(d,c);if(!f||!init_egl())return 0;
    int width=0,height=0;for(const int *q=a;q&&*q;q+=2){
        if(*q==GLX_PBUFFER_WIDTH)width=q[1];else if(*q==GLX_PBUFFER_HEIGHT)height=q[1];
        else if(*q!=GLX_PRESERVED_CONTENTS&&*q!=GLX_LARGEST_PBUFFER)return 0;
    }
    if(width<=0||height<=0)return 0;
    struct pbuffer *p=calloc(1,sizeof(*p));if(!p)return 0;
    EGLint attrs[]={EGL_WIDTH,width,EGL_HEIGHT,height,EGL_NONE};
    p->surface=eglCreatePbufferSurface(edpy,ecfg,attrs);
    if(p->surface==EGL_NO_SURFACE){free(p);return 0;}
    p->id=XCreatePixmap(d,XRootWindow(d,f->visual.screen),1,1,f->visual.depth);
    if(!p->id){eglDestroySurface(edpy,p->surface);free(p);return 0;}
    p->display=d;p->width=width;p->height=height;p->refs=1;
    contexts_lock_acquire();p->next=pbuffers;pbuffers=p;contexts_lock_release();return p->id;
}
static void glXDestroyPbuffer(Display*d,GLXPbuffer id){
    contexts_lock_acquire();struct pbuffer **link=&pbuffers;
    while(*link&&((*link)->display!=d||(*link)->id!=id))link=&(*link)->next;
    struct pbuffer *p=*link;if(p)*link=p->next;contexts_lock_release();
    if(p){XFreePixmap(d,id);put_pbuffer(p);}
}
static int glXQueryContext(Display*d,GLXContext c,int a,int*v){(void)d;if(!c||!v)return 1;*v=a==GLX_RENDER_TYPE?GLX_RGBA_BIT:0;return 0;}
static void glXSelectEvent(Display*d,GLXDrawable w,unsigned long m){(void)d;(void)w;(void)m;}
static void glXGetSelectedEvent(Display*d,GLXDrawable w,unsigned long*m){(void)d;(void)w;if(m)*m=0;}
static void *dispatch(const GLubyte*n){if(!n)return 0;
#define M(f) if(!strcmp((const char*)n,#f))return (void*)f
M(glXQueryExtension);M(glXQueryVersion);M(glXCopyContext);M(glXCreateGLXPixmap);M(glXDestroyGLXPixmap);M(glXUseXFont);M(glXGetCurrentDisplay);M(glXCreateWindow);M(glXDestroyWindow);M(glXCreatePixmap);M(glXDestroyPixmap);M(glXCreatePbuffer);M(glXDestroyPbuffer);M(glXQueryContext);M(glXSelectEvent);M(glXGetSelectedEvent);M(glXGetClientString);M(glXQueryServerString);M(glXQueryExtensionsString);M(glXChooseFBConfig);M(glXGetFBConfigs);M(glXGetFBConfigAttrib);M(glXGetVisualFromFBConfig);M(glXChooseVisual);M(glXGetConfig);M(glXCreateContext);M(glXCreateNewContext);M(glXCreateContextAttribsARB);M(glXMakeCurrent);M(glXMakeContextCurrent);M(glXGetCurrentContext);M(glXGetCurrentDrawable);M(glXGetCurrentReadDrawable);M(glXSwapBuffers);M(glXDestroyContext);M(glXIsDirect);M(glXWaitGL);M(glXWaitX);M(glXQueryDrawable);M(glXSwapIntervalEXT);M(glXSwapIntervalMESA);M(glXGetSwapIntervalMESA);M(glXSwapIntervalSGI);M(glXGetProcAddress);M(glXGetProcAddressARB);
#undef M
return 0;}
static void set_index(const GLubyte*n,int i){(void)n;(void)i;}
EXPORT Bool __glx_Main(uint32_t version,const __GLXapiExports*exports,__GLXvendorInfo*v,__GLXapiImports*imports){(void)v;if(GLX_VENDOR_ABI_GET_MAJOR_VERSION(version)!=GLX_VENDOR_ABI_MAJOR_VERSION||!exports||!imports)return 0;memset(imports,0,sizeof(*imports));imports->isScreenSupported=supported;imports->getProcAddress=getproc;imports->getDispatchAddress=dispatch;imports->setDispatchIndex=set_index;return 1;}
