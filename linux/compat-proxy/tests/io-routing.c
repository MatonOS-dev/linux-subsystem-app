/* Inherited filters must permit native linker I/O on private descriptors. */
#include "../proxy.c"
static volatile unsigned traps;
#define CHECK(x) do {if(!(x))RAW(PROXY_SC_exit_group,__LINE__,0,0,0,0,0);} while(0)
__attribute__((noinline)) static long payload_io(long n,long fd,void *buf,long len){
 long r;
 __asm__ volatile("xor %%r10d,%%r10d; syscall":"=a"(r):"a"(n),"D"(fd),"S"(buf),"d"(len):"rcx","r11","r10","memory");
 return r;
}
static void probe(int sig,siginfo_t *info,void *ctx){
 (void)sig;(void)info;traps++;ARCH_CONTEXT(ctx,ARCH_RESULT)=-EBADF;
}
int main(void){
 CHECK(proxy_diagnostics_init()==0);
 long fd=RAW(PROXY_SC_memfd_create,"io-routing",0,0,0,0,0);CHECK(fd>=0);
 struct kernel_action action={(uintptr_t)probe,SA_SIGINFO|SA_RESTORER,(uintptr_t)proxy_restore,0};
 CHECK(RAW(PROXY_SC_rt_sigaction,SIGSYS,&action,0,8,0,0)==0);
 CHECK(RAW(PROXY_SC_prctl,PR_SET_NO_NEW_PRIVS,1,0,0,0,0)==0);
 CHECK(RAW(PROXY_SC_dup3,fd,proxy_fd_reserve,0,0,0,0)==proxy_fd_reserve);
 struct arch_ip_range native={(uintptr_t)payload_io,(uintptr_t)payload_io+256,0};
 CHECK(arch_app_install()==0 && arch_filter_install(&native,1)==0);
 char c='x';struct iovec v={&c,1};
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_pwrite64),proxy_fd_reserve,&c,1)==1);
 c=0;CHECK(payload_io(arch_encode_syscall(PROXY_SC_pread64),proxy_fd_reserve,&c,1)==1 && c=='x');
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_writev),proxy_fd_reserve,&v,1)==1);
 CHECK(traps==0);
 const char msg[]="PASS native-range positional I/O and writev on private fd bypass routing\n";
 RAW(PROXY_SC_write,1,msg,sizeof(msg)-1,0,0,0);RAW(PROXY_SC_exit_group,0,0,0,0,0,0);
 __builtin_unreachable();
}
