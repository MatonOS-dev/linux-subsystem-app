/* Production signal-handler and clone-retry path with resident adoption. */
#include "../arch/x86_64/thread-adopt.c"
#include "../proxy.h"
#include <string.h>
extern char **environ;
int main(int argc,char **argv) {
 if(argc>1 && !strcmp(argv[1],"--trace")){proxy_trace=1;argc--;argv++;}
 int raw_fork=argc>1 && !strcmp(argv[1],"--raw-fork");
 if(raw_fork){argc--;argv++;}
 if(argc<2)return 2;
 if(proxy_install(1)||adopt_install())return 3;
 if(raw_fork){
  /* A native clone continuation can reach exec before its adoption hook.
   * Preserve the parent record deliberately, as in the device bwrap child. */
  atomic_store(&adopt_current()->payload,1);
  long child=R(fork,0,0,0,0,0,0);
  if(child<0)return 5;
  if(child){int status;return R(wait4,child,&status,0,0,0,0)==child && status==0?0:6;}
 }
 struct image image;
 if(proxy_load(argv[1],argv+1,environ,&image))return 4;
 adopt_launch(image.entry,image.stack);
}
