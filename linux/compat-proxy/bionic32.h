/* SPDX-License-Identifier: Apache-2.0
 * Session-wide ELF32 native-runtime reservation. Kept across every exec;
 * only native runtime code is mapped here, never the Linux payload. */
#ifndef MATON_BIONIC32_H
#define MATON_BIONIC32_H
#define BIONIC32_SIZE UINT32_C(0x10000000)
#define BIONIC32_ENV "MATON_V3_BIONIC32_BASE="
extern uint32_t proxy_bionic32_base;
/* Prefix every inherited filter, before payload fd/signal rules. The peer
 * linker runs before a SIGSYS handler exists, including its libc constructors. */
static inline unsigned bionic32_filter(struct sock_filter *p) {
    if(!proxy_bionic32_base)return 0;
    const struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_I386,0,7),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)+4),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0,0,5),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,UINT32_C(0x70300102),2,0),
        BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,proxy_bionic32_base,0,2),
        BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,proxy_bionic32_base+BIONIC32_SIZE,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    for(unsigned i=0;i<sizeof(code)/sizeof(code[0]);i++)p[i]=code[i];
    return sizeof(code)/sizeof(code[0]);
}
#endif
