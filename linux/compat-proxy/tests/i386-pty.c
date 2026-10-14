/* Original compat tty fixture; exercises the same generic termios2 adapter. */
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
extern int grantpt(int),unlockpt(int);
extern char *ptsname(int);
#include <sys/ioctl.h>
struct term2 {uint32_t i,o,c,l;uint8_t line,cc[19];uint32_t is,os;};
int main(int argc,char **argv,char **env){
 (void)argc;(void)argv;(void)env;
 int m=open("/dev/ptmx",O_RDWR|O_NOCTTY);if(m<0)return 51;
 if(grantpt(m) || unlockpt(m))return 52;
 char *name=ptsname(m);if(!name)return 53;
 int s=open(name,O_RDWR|O_NOCTTY);if(s<0)return 54;
 struct term2 t;if(ioctl(s,0x802c542a,&t) || ioctl(s,0x402c542b,&t))return 55;
 close(s);close(m);static const char ok[]="PASS i386 pty and termios2\n";write(1,ok,sizeof(ok)-1);return 0;
}
