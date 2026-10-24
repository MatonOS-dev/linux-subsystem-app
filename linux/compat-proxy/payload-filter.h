/* SPDX-License-Identifier: Apache-2.0 */
/* Bionic's ELF32 startup queries personality and selects PER_LINUX32.
 * Flatpak allows only PER_LINUX and otherwise returns EPERM, before this
 * process can install its i386 proxy. Keep these two native startup calls
 * usable; Android's inherited app policy still applies. This is compatibility
 * routing, not an in-process security boundary. */
static long payload_filter(long nr,proxy_arg_t *a) {
#ifdef MATON_GPU_BRIDGE
    const int slot=2;
    if(state_gpu_bridge &&
       ((nr==PROXY_SC_prctl && a[0]==PR_SET_SECCOMP && a[1]==SECCOMP_MODE_FILTER) ||
        (nr==PROXY_SC_seccomp && a[0]==SECCOMP_SET_MODE_FILTER))) {
        struct sock_fprog original;
        if(proxy_copy(&original,(void *)(uintptr_t)a[slot],sizeof(original)))return -EFAULT;
        struct sock_filter prefix[]={
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_I386,0,8),
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,136,0,6),
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[0])+4),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0,0,4),
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[0])),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,UINT32_MAX,1,0),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,8,0,1),
            BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
            BPF_STMT(BPF_LD|BPF_IMM,0),
        };
        size_t count=sizeof(prefix)/sizeof(prefix[0]);
        if(!original.len || original.len>4096-count)return -EINVAL;
        struct sock_filter *code=vfs_alloc((original.len+count)*sizeof(*code));
        if(!code)return -ENOMEM;
        p_copy(code,prefix,sizeof(prefix));
        long result=-EFAULT;
        if(!proxy_copy(code+count,original.filter,original.len*sizeof(*code))) {
            struct sock_fprog program={.len=original.len+count,.filter=code};
            result=RAW(nr,a[0],a[1],&program,a[3],a[4],a[5]);
        }
        vfs_free(code);return result;
    }
#endif
    return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
}
