#include <ucontext.h>
#include <elf.h>
#include <linux/audit.h>
#include "syscalls.h"
struct kernel_action { uintptr_t handler,flags,restorer; unsigned long mask; };
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

#endif
