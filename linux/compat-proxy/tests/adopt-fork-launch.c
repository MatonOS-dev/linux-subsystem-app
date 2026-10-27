/* Production signal-handler and clone-retry path with resident adoption. */
#include "../arch/x86_64/thread-adopt.c"
#include "../proxy.h"
#include <string.h>
extern char **environ;
int main(int argc,char **argv) {
 if(argc>1 && !strcmp(argv[1],"--trace")){proxy_trace=1;argc--;argv++;}
 if(argc<2)return 2;
 if(proxy_install(1)||adopt_install())return 3;
 struct image image;
 if(proxy_load(argv[1],argv+1,environ,&image))return 4;
 adopt_launch(image.entry,image.stack);
}
