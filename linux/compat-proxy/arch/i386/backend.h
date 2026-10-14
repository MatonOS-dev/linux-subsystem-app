#include <signal.h>
#include <ucontext.h>
#include <elf.h>
#include <linux/audit.h>
#include "syscalls.h"
struct kernel_action { uintptr_t handler,flags,restorer; uint64_t mask; };
#define ARCH_CLEAR_TLS() __asm__ volatile("xor %%eax,%%eax; mov %%ax,%%gs" ::: "eax")
#define ARCH_AUDIT AUDIT_ARCH_I386
#define ARCH_ELF_CLASS ELFCLASS32
#define ARCH_ELF_MACHINE EM_386
#define ARCH_PLATFORM "i686"
#define ARCH_RESIDENT_LO 0x70000000UL
#define ARCH_RESIDENT_HI 0x74000000UL
typedef Elf32_Ehdr arch_elf_ehdr;
typedef Elf32_Phdr arch_elf_phdr;
typedef Elf32_auxv_t arch_elf_auxv;
#ifdef ARCH_WITH_CONTEXT
static inline greg_t *arch_context_slot(void *context, enum arch_slot slot) {
    static const int indices[] = { REG_EAX, REG_EIP, REG_ESP, REG_EBX,
        REG_ECX, REG_EDX, REG_ESI, REG_EDI, REG_EBP };
    return &((ucontext_t *)context)->uc_mcontext.gregs[indices[slot]];
}
#define ARCH_CONTEXT(context,slot) (*arch_context_slot((context),(slot)))
static inline void arch_restart_syscall(void *context) {
    ARCH_CONTEXT(context,ARCH_PC)-=2; /* int 0x80 instruction; vDSO retry is handled separately below. */
}

static inline void arch_exec_context(void *context){
 ARCH_CONTEXT(context,ARCH_ARG2)=0;
 ((ucontext_t *)context)->uc_mcontext.gregs[REG_GS]=0;
 ((ucontext_t *)context)->uc_mcontext.gregs[REG_FS]=0;
}
/* The kernel restores this mask and altstack at rt_sigreturn. */
static inline uint64_t *arch_signal_mask(void *context) {
    return (uint64_t *)&((ucontext_t *)context)->uc_sigmask;
}
static inline void *arch_signal_stack(void *context) {
    return &((ucontext_t *)context)->uc_stack;
}
static inline void arch_signal_stack_ready(void *context) {
    ((ucontext_t *)context)->uc_stack.ss_flags &= ~SS_ONSTACK;
}
#endif

/* BPF matches the kernel-reported return PC, not a guessed opcode length. */
static inline uint64_t arch_gateway_pc(unsigned which) {
    extern char proxy_raw_after[], proxy_clone_after[];
    return (uintptr_t)(which ? proxy_clone_after : proxy_raw_after);
}

#define ARCH_POLICY_HEADER "build/app-policy-i386.h"

#include <sys/stat.h>
_Static_assert(sizeof(struct stat)==96,"kernel stat wire size");
_Static_assert(__builtin_offsetof(struct stat,st_ino)==88,"kernel stat inode offset");

#include <errno.h>
static inline int arch_trap_number(long nr){return nr==102 || nr==117;}
static inline struct arch_call arch_decode_call(long nr,proxy_arg_t *a){
 struct arch_call c={.id=arch_decode_syscall(nr)};
 if(nr==192)a[5]=(uint64_t)(uint32_t)a[5]<<12;
 if(nr==180 || nr==181){a[3]=(uint64_t)(uint32_t)a[3]|((uint64_t)(uint32_t)a[4]<<32);a[4]=0;}
 if(nr==194){c.id=PROXY_SC_ftruncate;a[1]=(uint64_t)(uint32_t)a[1]|((uint64_t)(uint32_t)a[2]<<32);a[2]=0;}
 if(nr==191)c.id=PROXY_SC_getrlimit;
 if(nr==394)a[2]&=~0x100;
 if(nr==396 || nr==402)a[1]&=~0x100;
 if(nr==102){
  static const long ops[]={PROXY_SC_unknown,PROXY_SC_socket,PROXY_SC_bind,
   PROXY_SC_connect,PROXY_SC_listen,PROXY_SC_accept,PROXY_SC_getsockname,
   PROXY_SC_getpeername,PROXY_SC_socketpair,PROXY_SC_sendto,PROXY_SC_recvfrom,
   PROXY_SC_sendto,PROXY_SC_recvfrom,PROXY_SC_shutdown,PROXY_SC_setsockopt,
   PROXY_SC_getsockopt,PROXY_SC_sendmsg,PROXY_SC_recvmsg,PROXY_SC_accept4,
   PROXY_SC_recvmmsg,PROXY_SC_sendmmsg};
  static const unsigned char words[]={0,3,3,3,2,3,3,3,4,4,4,6,6,2,5,5,3,3,4,5,4};
  unsigned op=a[0];uint32_t w[6]={0};
  if(!op || op>=sizeof(ops)/sizeof(ops[0])){c.id=-1;return c;}
  if(proxy_copy(w,(void *)(uintptr_t)a[1],words[op]*4)){c.id=-1;c.error=-EFAULT;return c;}
  for(unsigned i=0;i<6;i++)a[i]=(int32_t)w[i];c.id=ops[op];return c;
 }
 if(nr==117){
  proxy_arg_t w[6];for(unsigned i=0;i<6;i++)w[i]=a[i];
  unsigned op=(unsigned)w[0]&0xffff,version=(unsigned)w[0]>>16;
  for(unsigned i=0;i<6;i++)a[i]=0;
  a[0]=w[1];
  switch(op){
   case 1:c.id=PROXY_SC_semop;a[1]=w[4];a[2]=w[2];break;
   case 2:c.id=PROXY_SC_semget;a[1]=w[2];a[2]=w[3];break;
   case 3:{uint32_t arg;if(proxy_copy(&arg,(void *)(uintptr_t)w[4],4)){c.error=-EFAULT;break;}
    c.id=PROXY_SC_semctl;a[1]=w[2];a[2]=w[3]&~0x100;a[3]=(int32_t)arg;break;}
   case 4:c.id=PROXY_SC_semtimedop;a[1]=w[4];a[2]=w[2];a[3]=w[5];break;
   case 11:c.id=PROXY_SC_msgsnd;a[1]=w[4];a[2]=w[2];a[3]=w[3];break;
   case 12:c.id=PROXY_SC_msgrcv;a[1]=w[4];a[2]=w[2];a[3]=w[5];a[4]=w[3];
    if(!version){int32_t kludge[2];if(proxy_copy(kludge,(void *)(uintptr_t)w[4],8)){c.error=-EFAULT;break;}
     a[1]=kludge[0];a[3]=kludge[1];}break;
   case 13:c.id=PROXY_SC_msgget;a[1]=w[2];break;
   case 14:c.id=PROXY_SC_msgctl;a[1]=w[2]&~0x100;a[2]=w[4];break;
   case 21:c.id=PROXY_SC_shmat;a[1]=w[4];a[2]=w[2];c.variant=1;c.output=(uintptr_t)w[3];break;
   case 22:c.id=PROXY_SC_shmdt;a[0]=w[4];break;
   case 23:c.id=PROXY_SC_shmget;a[1]=w[2];a[2]=w[3];break;
   case 24:c.id=PROXY_SC_shmctl;a[1]=w[2]&~0x100;a[2]=w[4];break;
   default:c.id=-1;break;
  }
 }
 return c;
}
static inline long arch_finish_call(struct arch_call c,long result){
 if(c.error)return c.error;
 if(c.variant==1 && (unsigned long)result<(unsigned long)-4095){
  uint32_t address=(uint32_t)result;
  return proxy_copy((void *)c.output,&address,4);
 }
 return result;
}

/* Internal offsets are bytes. Decode mmap2 before the core sees the call. */
static inline long arch_raw_call(long id,proxy_arg_t a,proxy_arg_t b,proxy_arg_t c,
 proxy_arg_t d,proxy_arg_t e,proxy_arg_t f){
 if(id==PROXY_SC_mmap){if(f<0 || ((uint64_t)f&4095))return -EINVAL;f=(uint64_t)f>>12;}
 if(id==PROXY_SC_ftruncate)return proxy_raw(194,a,(uint32_t)b,(uint64_t)b>>32,0,0,0);
 if(id==PROXY_SC_pread64 || id==PROXY_SC_pwrite64){e=(uint64_t)d>>32;d=(uint32_t)d;}
 if(id==PROXY_SC_lseek){uint64_t out=0;
  long r=proxy_raw(140,a,(uint64_t)b>>32,(uint32_t)b,(long)&out,c,0);
  if(r)return r;
  return out>INT32_MAX?-EOVERFLOW:(long)out;
 }
 return proxy_raw(arch_encode_syscall(id),a,b,c,d,e,f);
}

#define ARCH_IPC_SEM_TIMES(w,a,c) do { (w).sem_otime=(a); (w).sem_otime_high=(uint64_t)(a)>>32; (w).sem_ctime=(c); (w).sem_ctime_high=(uint64_t)(c)>>32; } while(0)

#define ARCH_IPC_SHM_TIMES(w,a,d,c) do { (w).shm_atime=(a); (w).shm_atime_high=(uint64_t)(a)>>32; (w).shm_dtime=(d); (w).shm_dtime_high=(uint64_t)(d)>>32; (w).shm_ctime=(c); (w).shm_ctime_high=(uint64_t)(c)>>32; } while(0)

#define ARCH_IPC_MSG_TIMES(w,a,d,c) do { (w).msg_stime=(a); (w).msg_stime_high=(uint64_t)(a)>>32; (w).msg_rtime=(d); (w).msg_rtime_high=(uint64_t)(d)>>32; (w).msg_ctime=(c); (w).msg_ctime_high=(uint64_t)(c)>>32; } while(0)

#include <sys/socket.h>
#include <sys/uio.h>
_Static_assert(sizeof(struct msghdr)==28,"msghdr wire size");
_Static_assert(sizeof(struct iovec)==8,"iovec wire size");
_Static_assert(sizeof(struct cmsghdr)==12,"cmsg wire size");

#define ARCH_AUX_SYSINFO 32

#define ARCH_PEER_SYSCALLS "x86_64/syscalls.h"

#define ARCH_FILTER_HEADER "arch/x86-filter.h"

#define ARCH_PEER_ELF_CLASS ELFCLASS64
#define ARCH_PEER_ELF_MACHINE EM_X86_64
#define ARCH_PEER_LOADER "libmatonos-v3-loader.so"
#define ARCH_PEER_HOST_LOADER "linux-layer-v3"
#include "../exec.h"
