#define _GNU_SOURCE
#define MATON_GPU_BRIDGE
#include "../proxy.h"
#include "../vfs.h"
#include "../state.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <sys/prctl.h>
#include <stddef.h>
int state_gpu_bridge;
static struct seccomp_data data;
static unsigned calls;
void *vfs_alloc(size_t n){return malloc(n);}
void vfs_free(void *p){free(p);}
void p_copy(void *d,const void *s,size_t n){memcpy(d,s,n);}
long proxy_copy(void *d,const void *s,size_t n){if(!s)return -EFAULT;memcpy(d,s,n);return 0;}
long proxy_raw(long nr,long a,long b,long c,long d,long e,long f){
    (void)d;(void)e;(void)f;
    assert((nr==157 && a==PR_SET_SECCOMP && b==SECCOMP_MODE_FILTER) ||
           (nr==317 && a==SECCOMP_SET_MODE_FILTER && b==SECCOMP_FILTER_FLAG_TSYNC));
    calls++;
    const struct sock_fprog *p=(void *)c;
    unsigned acc=0;
    for(unsigned pc=0;pc<p->len;pc++) {
        struct sock_filter ins=p->filter[pc];
        switch(ins.code) {
        case BPF_LD|BPF_W|BPF_ABS:assert(ins.k+4<=sizeof(data));memcpy(&acc,(char *)&data+ins.k,4);break;
        case BPF_LD|BPF_IMM:acc=ins.k;break;
        case BPF_JMP|BPF_JEQ|BPF_K:pc+=acc==ins.k?ins.jt:ins.jf;break;
        case BPF_JMP|BPF_JGE|BPF_K:pc+=acc>=ins.k?ins.jt:ins.jf;break;
        case BPF_RET|BPF_K:return ins.k;
        default:abort();
        }
    }
    abort();
}
#include "../payload-filter.h"
#include "../payload-native-filter.h"
int main(void){
    /* Native startup may execute with SIGSYS blocked. Its set_tid_address
     * must bypass a renderer TRAP filter; payload and foreign ABI must not. */
    struct sock_filter native[32];
    uint64_t lo=UINT64_C(0x792efffff000),hi=lo+8192;
    unsigned n=payload_native_range(native,lo,hi);
    assert(n==payload_native_range(NULL,lo,hi));
    native[n++]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP|1);
    struct sock_fprog native_program={n,native};
    data.arch=AUDIT_ARCH_X86_64;data.nr=218;
    for(uint64_t ip=lo-1;ip<=hi;ip++) {
        data.instruction_pointer=ip;
        long action=proxy_raw(317,SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_TSYNC,(long)&native_program,0,0,0);
        assert(action==(ip>=lo && ip<hi?SECCOMP_RET_ALLOW:(SECCOMP_RET_TRAP|1)));
    }
    data.arch=AUDIT_ARCH_I386;data.instruction_pointer=lo;
    assert(proxy_raw(317,SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_TSYNC,(long)&native_program,0,0,0)==(SECCOMP_RET_TRAP|1));
    calls=0;
    struct sock_filter deny[]={BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM)};
    struct sock_fprog program={1,deny};
    proxy_arg_t prctl_args[]={PR_SET_SECCOMP,SECCOMP_MODE_FILTER,(uintptr_t)&program,0,0,0};
    proxy_arg_t seccomp_args[]={SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_TSYNC,(uintptr_t)&program,0,0,0};
    state_gpu_bridge=1;data.arch=AUDIT_ARCH_I386;data.nr=136;data.args[0]=UINT32_MAX;
    assert(payload_filter(PROXY_SC_prctl,prctl_args)==SECCOMP_RET_ALLOW);
    data.args[0]=8;assert(payload_filter(PROXY_SC_seccomp,seccomp_args)==SECCOMP_RET_ALLOW);
    data.args[0]=0x40008;assert(payload_filter(PROXY_SC_prctl,prctl_args)==(SECCOMP_RET_ERRNO|EPERM));
    data.args[0]=UINT64_C(0x100000008);assert(payload_filter(PROXY_SC_prctl,prctl_args)==(SECCOMP_RET_ERRNO|EPERM));
    data.args[0]=UINT32_MAX;data.nr=243;assert(payload_filter(PROXY_SC_prctl,prctl_args)==(SECCOMP_RET_ERRNO|EPERM));
    data.nr=136;data.arch=AUDIT_ARCH_X86_64;assert(payload_filter(PROXY_SC_prctl,prctl_args)==(SECCOMP_RET_ERRNO|EPERM));
    data.arch=AUDIT_ARCH_I386;state_gpu_bridge=0;assert(payload_filter(PROXY_SC_prctl,prctl_args)==(SECCOMP_RET_ERRNO|EPERM));
    unsigned before=calls;state_gpu_bridge=1;program.len=4096;
    assert(payload_filter(PROXY_SC_prctl,prctl_args)==-EINVAL && calls==before);
    program.len=1;prctl_args[2]=0;
    assert(payload_filter(PROXY_SC_prctl,prctl_args)==-EFAULT && calls==before);
    puts("PASS ELF32 bionic personality startup routing; other flags/ABIs/syscalls and filter errors preserved");
}
