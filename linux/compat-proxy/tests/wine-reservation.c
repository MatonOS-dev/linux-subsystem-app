/* Exercise the low address reservations Wine's preloader expects to own. */
typedef unsigned long uptr;
typedef long isize;

#if defined(__x86_64__)
#define SC_mmap 9
#define SC_openat 257
#define SC_read 0
#define SC_write 1
#define SC_close 3
#define AT_FDCWD -100
static isize call6(isize n,isize a,isize b,isize c,isize d,isize e,isize f){
 register isize r10 __asm__("r10")=d;register isize r8 __asm__("r8")=e;register isize r9 __asm__("r9")=f;
 isize r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");return r;
}
#else
#define SC_mmap 192
#define SC_openat 295
#define SC_read 3
#define SC_write 4
#define SC_close 6
#define AT_FDCWD -100
static isize call6(isize n,isize a,isize b,isize c,isize d,isize e,isize f){
 isize r; __asm__ volatile("push %%ebp; mov %7,%%ebp; int $0x80; pop %%ebp":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c),"S"(d),"D"(e),"r"(f):"memory");return r;
}
#endif
static isize call3(isize n,isize a,isize b,isize c){return call6(n,a,b,c,0,0,0);}
static void out(const char *s,isize n){call3(SC_write,1,(isize)s,n);}
static void maps(void){
 char b[4096];isize fd=call6(SC_openat,AT_FDCWD,(isize)"/proc/self/maps",0,0,0,0);
 if(fd<0)return;
 isize n;while((n=call3(SC_read,fd,(isize)b,sizeof(b)))>0)out(b,n);call3(SC_close,fd,0,0);
}
int main(void){
 const uptr starts[]={0x10000,0x110000,0x7f000000,
#if defined(__x86_64__)
  0x7ffffe000000UL
#else
  0
#endif
 };
 const uptr ends[]={0x110000,0x68000000,0x7fff0000,
#if defined(__x86_64__)
  0x7ffffe001000UL
#else
  0
#endif
 };
 for(unsigned i=0;i<4;i++){
  if(!starts[i])continue;
  uptr len=ends[i]-starts[i];
  isize r=call6(SC_mmap,starts[i],len,0,0x100022,-1,0);
  if(r<0){static const char msg[]="FAIL reservation; /proc/self/maps follows\n";out(msg,sizeof(msg)-1);maps();return 1;}
 }
 static const char msg[]="PASS Wine reservation ranges available\n";out(msg,sizeof(msg)-1);return 0;
}
