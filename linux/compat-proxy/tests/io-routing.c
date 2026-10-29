/* Production-filter regression: ordinary read/write/vector I/O must not
 * enter SIGSYS. The private window must retain its existing route. */
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
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_pwrite64),fd,&c,1)==1);
 c=0;CHECK(payload_io(arch_encode_syscall(PROXY_SC_pread64),fd,&c,1)==1 && c=='x');
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_writev),fd,&v,1)==1);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_write),fd,&c,1)==1);
 CHECK(RAW(PROXY_SC_lseek,fd,0,SEEK_SET,0,0,0)==0);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_read),fd,&c,1)==1);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_readv),fd,&v,1)==1);
 CHECK(traps==0);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_pread64),proxy_fd_reserve,&c,1)==-EBADF);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_pwrite64),proxy_fd_reserve_end-1,&c,1)==-EBADF);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_writev),proxy_fd_reserve,&v,1)==-EBADF);
 CHECK(traps==3);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_pread64),-1,&c,1)==-EBADF && traps==3);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_read),io_window_begin(),&c,1)==-EBADF);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_write),io_window_begin(),&c,1)==-EBADF);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_readv),io_window_begin(),&v,1)==-EBADF);
 CHECK(traps==6);
 CHECK(io_alias_route((int)fd)==0);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_read),fd,&c,1)==-EBADF && traps==7);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_write),fd,&c,1)==-EBADF && traps==8);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_readv),fd,&v,1)==-EBADF && traps==9);
 long ordinary=RAW(PROXY_SC_memfd_create,"ordinary",0,0,0,0,0);CHECK(ordinary>=0 && ordinary!=fd);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_write),ordinary,&c,1)==1 && traps==9);
 proxy_arg_t duplicate_args[6]={fd,0,0,0,0,0};
 long ordinary_dup=descriptor_dup_dispatch(PROXY_SC_dup,duplicate_args);CHECK(ordinary_dup>=0);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_write),ordinary_dup,&c,1)==1 && traps==9);
 long inherited=RAW(PROXY_SC_inotify_init1,IN_NONBLOCK,0,0,0,0,0);CHECK(inherited>=0 && inherited<io_window_begin());
 CHECK(io_adopt_inherited()==0);
 CHECK(payload_io(arch_encode_syscall(PROXY_SC_read),inherited,&c,1)==-EBADF && traps==10);
 const char msg[]="PASS ordinary read/write/vector I/O bypass SIGSYS; special window and exact aliases trap\n";
 RAW(PROXY_SC_write,1,msg,sizeof(msg)-1,0,0,0);RAW(PROXY_SC_exit_group,0,0,0,0,0,0);
 __builtin_unreachable();
}
