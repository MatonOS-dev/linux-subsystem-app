/* SPDX-License-Identifier: Apache-2.0 */
/* Only native x86_64 resident mappings qualify, even in the i386 proxy.
 * Kernel exec reserves inherited ranges before loading another payload. */
static inline unsigned payload_native_range(struct sock_filter *out,
                                            uint64_t lo,uint64_t end) {
    unsigned n=0;
    while(lo<end) {
        uint64_t hi=((lo>>32)+1)<<32;
        if(!hi || hi>end)hi=end;
        int capped=(uint32_t)hi!=0;
        struct sock_filter code[]={
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,0,capped?6:5),
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)+4),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,(uint32_t)(lo>>32),0,capped?4:3),
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)),
            BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,(uint32_t)lo,0,capped?2:1),
            BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,(uint32_t)hi,1,0),
            BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        };
        for(unsigned i=0;i<8;i++) {
            if(!capped && i==6)continue;
            if(out)out[n]=code[i];
            n++;
        }
        lo=hi;
    }
    return n;
}
