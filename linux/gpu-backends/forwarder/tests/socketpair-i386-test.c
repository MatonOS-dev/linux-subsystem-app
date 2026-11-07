/* Runs the real i386 backend helpers as a static freestanding i386 binary:
 * mv_socketpair must return 0 and give two connected SEQPACKET endpoints
 * (regression: socketcall op 1 = socket left pair[] unset), and
 * mv_monotonic_ns must be nonzero and non-decreasing. */
#include "backend.h"

long mv_sys(long n,long a,long b,long c,long d,long e,long f) { return mv_native_sys(n,a,b,c,d,e,f); }

static void out(const char *s) { size_t n=0; while(s[n])n++; mv_write(s,n); }
static __attribute__((noreturn)) void finish(int code) {
    for(;;) (void)mv_native_sys(1,code,0,0,0,0,0);
}
static void check(int ok,const char *what) { if(!ok){ out("FAIL i386 backend: "); out(what); out("\n"); finish(1); } }

static int test_main(void) {
    int pair[2]={-1,-1};
    check(mv_socketpair(pair)==0,"mv_socketpair returns 0");
    check(pair[0]>=0&&pair[1]>=0&&pair[0]!=pair[1],"mv_socketpair fills both fds");
    char a[]="ping",b[8]={0};
    check(mv_write_fd(pair[0],a,4)==4,"write end 0");
    check(mv_read(pair[1],b,sizeof(b))==4&&b[0]=='p'&&b[3]=='g',"read end 1");
    check(mv_write_fd(pair[1],"pong",4)==4,"write end 1");
    check(mv_read(pair[0],b,sizeof(b))==4&&b[0]=='p'&&b[1]=='o',"read end 0");
    mv_close(pair[0]); mv_close(pair[1]);
    uint64_t t0=mv_monotonic_ns(),t1=mv_monotonic_ns();
    check(t0!=0&&t1>=t0,"mv_monotonic_ns");
    out("PASS i386 backend socketpair + monotonic clock\n");
    return 0;
}

__attribute__((force_align_arg_pointer)) void _start(void) { finish(test_main()); }
