#include <signal.h>
#include <ucontext.h>
#include <elf.h>
#include <linux/audit.h>
#include "syscalls.h"
struct kernel_action { uintptr_t handler,flags,restorer; uint64_t mask; };
#define ARCH_CLEAR_TLS() RAW(PROXY_SC_arch_prctl,0x1002,0,0,0,0,0)
#define ARCH_AUDIT AUDIT_ARCH_X86_64
#define ARCH_ELF_CLASS ELFCLASS64
#define ARCH_ELF_MACHINE EM_X86_64
#define ARCH_PLATFORM "x86_64"
#define ARCH_RESIDENT_LO 0x600000000000UL
#define ARCH_RESIDENT_HI 0x600004000000UL
typedef Elf64_Ehdr arch_elf_ehdr;
typedef Elf64_Phdr arch_elf_phdr;
typedef Elf64_auxv_t arch_elf_auxv;
#ifdef ARCH_WITH_CONTEXT
static inline greg_t *arch_context_slot(void *context, enum arch_slot slot) {
    static const int indices[] = { REG_RAX, REG_RIP, REG_RSP, REG_RDI,
        REG_RSI, REG_RDX, REG_R10, REG_R8, REG_R9 };
    return &((ucontext_t *)context)->uc_mcontext.gregs[indices[slot]];
}
#define ARCH_CONTEXT(context,slot) (*arch_context_slot((context),(slot)))
static inline void arch_restart_syscall(void *context) {
    ARCH_CONTEXT(context,ARCH_PC)-=2; /* x86_64 syscall instruction */
}

static inline void arch_exec_context(void *context){
 ARCH_CONTEXT(context,ARCH_ARG2)=0;
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
    extern char proxy_raw_after[], proxy_clone_after[], proxy_raw_clone_after[];
    return (uintptr_t)(which == 1 ? proxy_clone_after :
                       which == 2 ? proxy_raw_clone_after : proxy_raw_after);
}

#define ARCH_POLICY_HEADER "build/app-policy.h"

#include <sys/stat.h>
_Static_assert(sizeof(struct stat)==144,"kernel stat wire size");
_Static_assert(__builtin_offsetof(struct stat,st_ino)==8,"kernel stat inode offset");

static inline int arch_trap_number(long nr){(void)nr;return 0;}
static inline struct arch_call arch_decode_call(long nr,proxy_arg_t *a){
 (void)a;return (struct arch_call){.id=arch_decode_syscall(nr)};
}
static inline long arch_finish_call(struct arch_call call,long result){
 (void)call;return result;
}

static inline long arch_raw_call(long id,proxy_arg_t a,proxy_arg_t b,proxy_arg_t c,
 proxy_arg_t d,proxy_arg_t e,proxy_arg_t f){
 return proxy_raw(arch_encode_syscall(id),a,b,c,d,e,f);
}

#define ARCH_IPC_SEM_TIMES(w,a,c) do { (w).sem_otime=(a); (w).sem_ctime=(c); } while(0)

#define ARCH_IPC_SHM_TIMES(w,a,d,c) do { (w).shm_atime=(a); (w).shm_dtime=(d); (w).shm_ctime=(c); } while(0)

#define ARCH_IPC_MSG_TIMES(w,a,d,c) do { (w).msg_stime=(a); (w).msg_rtime=(d); (w).msg_ctime=(c); } while(0)

#include <sys/socket.h>
#include <sys/uio.h>
_Static_assert(sizeof(struct msghdr)==56,"msghdr wire size");
_Static_assert(sizeof(struct iovec)==16,"iovec wire size");
_Static_assert(sizeof(struct cmsghdr)==16,"cmsg wire size");

#define ARCH_AUX_SYSINFO 32

#define ARCH_PEER_SYSCALLS "i386/syscalls.h"

#define ARCH_FILTER_HEADER "arch/x86-filter.h"

#define ARCH_PEER_ELF_CLASS ELFCLASS32
#define ARCH_PEER_ELF_MACHINE EM_386
#define ARCH_PEER_LOADER "libmatonos-v3-loader-i386.so"
#define ARCH_PEER_HOST_LOADER "linux-layer-v3-i386"
#include "../exec.h"

#define ARCH_REWRITE_HEADER "arch/x86_64/rewrite.h"
