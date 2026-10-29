/* Avoid a host Android display service. Preserve the original env/auxv
 * placement: unsetenv() in the older GPU stand-in masked this regression. */
#include <stdlib.h>
__attribute__((constructor)) static void clean_environment(void) {
 char *p=getenv("LD_PRELOAD");if(p)*p=0;
 p=getenv("LD_LIBRARY_PATH");if(p)*p=0;
}
void *eglGetDisplay(void *display){(void)display;return 0;}
