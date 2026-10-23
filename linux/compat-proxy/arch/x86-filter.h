/* SPDX-License-Identifier: Apache-2.0
 * x86 family dual-ABI BPF implementation. The core supplies its common-ID
 * trap predicate; this backend alone chooses audit IDs, numbers and gateways. */
/* Both branches survive kernel exec. Never interpret a compat number using
 * the native table. JA (32-bit offset) avoids classic BPF's 8-bit branch limit. */
#include <linux/sockios.h>
static int arch_app_install(void) {
    struct sock_filter bpf[2048];unsigned n=0;
#define ADD(code,jt,jf,k) do {if(n>=sizeof(bpf)/sizeof(bpf[0])) return -E2BIG; bpf[n++]=(struct sock_filter)BPF_JUMP(code,k,jt,jf);} while(0)
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,arch));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,AUDIT_ARCH_X86_64);
    unsigned native_jump=n;ADD(BPF_JMP|BPF_JA,0,0,0);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,AUDIT_ARCH_I386);
    unsigned compat_jump=n;ADD(BPF_JMP|BPF_JA,0,0,0);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_KILL_PROCESS);
    for(unsigned branch=0;branch<2;branch++) {
        unsigned jump=branch?compat_jump:native_jump;bpf[jump].k=n-jump-1;
        const int *allowed=branch?android_i386_allowed:android_x86_64_allowed;
        unsigned count=branch?sizeof(android_i386_allowed)/sizeof(int):sizeof(android_x86_64_allowed)/sizeof(int);
        long (*encode_ioctl)(long)=branch?arch_peer_encode_syscall:arch_encode_syscall;
        ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,nr));
        /* Route interface ioctls through the proxy even when the Android
         * syscall allowlist would otherwise let ioctl reach the kernel. */
        ADD(BPF_JMP|BPF_JEQ|BPF_K,1,0,encode_ioctl(PROXY_SC_ioctl));
        ADD(BPF_JMP|BPF_JA,0,0,0);unsigned ioctl_jump=n-1;
        ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[1]));
        const unsigned ifreqs[]={SIOCGIFNAME,SIOCGIFCONF,SIOCGIFFLAGS,SIOCSIFFLAGS,
            SIOCGIFADDR,SIOCSIFADDR,SIOCGIFDSTADDR,SIOCSIFDSTADDR,
            SIOCGIFBRDADDR,SIOCSIFBRDADDR,SIOCGIFNETMASK,SIOCSIFNETMASK,
            SIOCGIFMETRIC,SIOCSIFMETRIC,SIOCGIFMEM,SIOCSIFMEM,
            SIOCGIFMTU,SIOCSIFMTU,SIOCGIFHWADDR,SIOCSIFHWADDR,
            SIOCGIFINDEX,SIOCSIFPFLAGS,SIOCGIFPFLAGS,SIOCGIFTXQLEN,SIOCSIFTXQLEN};
        for(unsigned i=0;i<sizeof(ifreqs)/sizeof(ifreqs[0]);i++) {
            ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,ifreqs[i]);
            /* The lower proxy filter traps ioctl and performs emulation.
             * This payload allowlist must not generate a second SIGSYS. */
            ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
        }
        bpf[ioctl_jump].k=n-ioctl_jump-1;
        ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,nr));
        for(unsigned i=0;i<count;i++) {
            ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,allowed[i]);ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
        }
        ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_TRAP);
    }
    struct sock_fprog prog={(unsigned short)n,bpf};
    return (int)RAW(PROXY_SC_seccomp,SECCOMP_SET_MODE_FILTER,0,&prog,0,0,0);
}
static int arch_filter_install(const struct arch_ip_range *libc_ranges, size_t libc_count) {
    /* The kernel's per-filter instruction limit is 4096. Grow to that limit
     * and fail cleanly if the process has more ranges than one filter can hold. */
    struct sock_filter bpf[4096];unsigned n=0;
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,arch));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,ARCH_AUDIT);
    unsigned native_jump=n;ADD(BPF_JMP|BPF_JA,0,0,0);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,ARCH_AUDIT==AUDIT_ARCH_I386?AUDIT_ARCH_X86_64:AUDIT_ARCH_I386);
    unsigned peer_jump=n;ADD(BPF_JMP|BPF_JA,0,0,0);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_KILL_PROCESS);
    for(unsigned peer=0;peer<2;peer++) {
        unsigned jump=peer?peer_jump:native_jump;bpf[jump].k=n-jump-1;
        long (*encode)(long)=peer?arch_peer_encode_syscall:arch_encode_syscall;
        long (*decode)(long)=peer?arch_peer_decode_syscall:arch_decode_syscall;
        unsigned audit=peer?(ARCH_AUDIT==AUDIT_ARCH_I386?AUDIT_ARCH_X86_64:AUDIT_ARCH_I386):ARCH_AUDIT;
        uint64_t gateway=audit==AUDIT_ARCH_I386?UINT64_C(0x70300000):UINT64_C(0x600000300000);
    /* x32 is unsupported independently of ioctl routing. */
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,nr));
    ADD(BPF_JMP|BPF_JSET|BPF_K,0,1,0x40000000);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_KILL_PROCESS);
    /* Raw proxy gateways must pass before the rt_sigaction guard below. */
    uint64_t ip=peer?gateway+0x42:arch_gateway_pc(0);
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,instruction_pointer)+4);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,3,(uint32_t)(ip>>32));
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,instruction_pointer));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,(uint32_t)ip);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    ip=peer?gateway+0x102:arch_gateway_pc(1);
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,instruction_pointer)+4);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,3,(uint32_t)(ip>>32));
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,instruction_pointer));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,(uint32_t)ip);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    /* Keep resident linker code from replacing the proxy's SIGSYS handler.
     * Let ordinary signal actions and read-only queries stay native. */
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,nr));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,7,encode(PROXY_SC_rt_sigaction));
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[0]));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,5,SIGSYS);
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[1]));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,2,0);
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[1])+4);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,1,0,0);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_TRAP);
    /* FD_CLOEXEC is kernel-owned state: let F_GETFD/F_SETFD reach Linux
     * directly so libc's million-descriptor spawn sweep does not trap.
     * Check the command's high word too; only the exact 32-bit commands
     * are exempt. Every other fcntl operation still updates proxy state. */
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,nr));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,6,encode(PROXY_SC_fcntl));
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[1])+4);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,4,0);
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[1]));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,1,0,F_GETFD);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,F_SETFD);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    if(resident_fds_enabled) {
        ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,nr));
        for(long id=0;id<PROXY_SC_count;id++)if(resident_fd_call(id) && id!=PROXY_SC_fcntl) {
            long native_nr=encode(id);if(native_nr<0)continue;
            ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,native_nr);
            ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_TRAP);
        }
        /* Exact F_GETFD/F_SETFD already passed above. */
        ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,encode(PROXY_SC_fcntl));
        ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_TRAP);
    }
    /* Bionic pthread startup installs its altstack while SIGSYS is blocked.
     * Its sigaltstack must pass through the resident ranges below, or the
     * synchronous trap kills the new thread before adoption can run. Payload
     * sigaltstack still traps through ours() after the resident exemption. */
    /* Resident bionic must be able to implement the Android-owned pthread,
     * allocator and signal machinery. This is a routing exemption only; the
     * Linux payload is not a security boundary. Bionic is native x86_64; never
     * grant its ranges to the i386 branch. */
    if(!peer) for(size_t r=0;r<libc_count;r++) {
        uint64_t lo=libc_ranges[r].begin, end=libc_ranges[r].end;
        /* Each BPF check compares one high word: split at 4 GiB boundaries. */
        for(uint64_t hi;lo<end;lo=hi) {
            hi=((lo>>32)+1)<<32; if(!hi || hi>end) hi=end;
            /* A piece ending on the boundary has no upper check in its low word. */
            int capped=(uint32_t)hi!=0;
            ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,instruction_pointer)+4);
            ADD(BPF_JMP|BPF_JEQ|BPF_K,0,capped?4:3,(uint32_t)(lo>>32));
            ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,instruction_pointer));
            ADD(BPF_JMP|BPF_JGE|BPF_K,0,capped?2:1,(uint32_t)lo);
            if(capped) ADD(BPF_JMP|BPF_JGE|BPF_K,1,0,(uint32_t)hi);
            ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
        }
    }
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,nr));
    /* Forks must account for inherited temporary descriptors. Threads retry natively. */
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,5,encode(PROXY_SC_clone));
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[0]));
    ADD(BPF_JMP|BPF_JSET|BPF_K,2,0,NS_FLAGS|CLONE_VFORK);
    ADD(BPF_JMP|BPF_JSET|BPF_K,0,1,CLONE_VM);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_TRAP);
    /* sendto without a destination stays native (both pointer halves). */
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,6,encode(PROXY_SC_sendto));
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[4]));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,3,0);
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[4])+4);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,0);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_TRAP);
    for(long nr=0;nr<512;nr++) if((ours(decode(nr)) || (audit==AUDIT_ARCH_I386 && (nr==102 || nr==117))) && nr!=encode(PROXY_SC_clone) && nr!=encode(PROXY_SC_sendto)) {
        ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,nr);ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_TRAP);
    }
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    }
    struct sock_fprog prog={(unsigned short)n,bpf};
    return (int)RAW(PROXY_SC_seccomp,SECCOMP_SET_MODE_FILTER,0,&prog,0,0,0);
}
#undef ADD
