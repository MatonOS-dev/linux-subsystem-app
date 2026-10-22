#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <glvnd/libglxabi.h>
static __GLXvendorInfo *get_dynamic(Display*d,int s){(void)d;(void)s;return 0;}
static __GLXvendorInfo *get_current(void){return 0;}
static __GLXextFuncPtr fetch(__GLXvendorInfo*v,int i){(void)v;(void)i;return 0;}
static GLXContext current(void){return 0;}
static int add_context(Display*d,GLXContext c,__GLXvendorInfo*v){(void)d;(void)c;(void)v;return 0;}
static void remove_context(Display*d,GLXContext c){(void)d;(void)c;}
static __GLXvendorInfo *from_context(GLXContext c){(void)c;return 0;}
static int add_fb(Display*d,GLXFBConfig c,__GLXvendorInfo*v){(void)d;(void)c;(void)v;return 0;}
static void remove_fb(Display*d,GLXFBConfig c){(void)d;(void)c;}
static __GLXvendorInfo *from_fb(Display*d,GLXFBConfig c){(void)d;(void)c;return 0;}
static int add_draw(Display*d,GLXDrawable x,__GLXvendorInfo*v){(void)d;(void)x;(void)v;return 0;}
static void remove_draw(Display*d,GLXDrawable x){(void)d;(void)x;}
static __GLXvendorInfo *from_draw(Display*d,GLXDrawable x){(void)d;(void)x;return 0;}
int main(int argc,char**argv){
 if(argc!=2)return 2;void*h=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!h){fprintf(stderr,"dlopen: %s\n",dlerror());return 1;}
 __PFNGLXMAINPROC main_fn=(__PFNGLXMAINPROC)dlsym(h,"__glx_Main");if(!main_fn)return 1;
 __GLXapiExports ex={get_dynamic,get_current,fetch,current,add_context,remove_context,from_context,add_fb,remove_fb,from_fb,add_draw,remove_draw,from_draw};
 __GLXapiImports in;memset(&in,0,sizeof(in));
 if(main_fn(0x00020000,&ex,0,&in))return 1;
 if(!main_fn(GLX_VENDOR_ABI_VERSION,&ex,0,&in)||!in.isScreenSupported||!in.getProcAddress||!in.getDispatchAddress||!in.setDispatchIndex)return 1;
 if(in.isScreenSupported(0,0))return 1;
 __GLXextFuncPtr q=(__GLXextFuncPtr)in.getDispatchAddress((const GLubyte*)"glXQueryVersion");
 if(!q)return 1; Bool (*query)(Display*,int*,int*)=(Bool(*)(Display*,int*,int*))q;int major=0,minor=0;
 if(query((Display*)1,&major,&minor))return 1;
 if(!in.getProcAddress((const GLubyte*)"glXChooseVisual")||!in.getProcAddress((const GLubyte*)"glXCreatePbuffer"))return 1;
 puts("PASS GLX vendor ABI handshake, dispatch lookup, and capability gating");dlclose(h);return 0;
}
