#ifndef MATON_GPU_BOOTSTRAP_H
#define MATON_GPU_BOOTSTRAP_H
#include <stdint.h>
/* Read-only named mapping; no fixed virtual address, fd, libc or TCB ABI. */
#define MATON_GPU_BOOTSTRAP_MAGIC UINT64_C(0x4d41544f4e475031)
#define MATON_GPU_BOOTSTRAP_NAME "maton-gpu-bootstrap-v2"
struct maton_gpu_bootstrap {
    uint64_t magic;
    char bridge[32], adopt[32], fast[32];
    /* Linux payload headers; /proc/self/auxv still describes the resident. */
    char phdr[32], phnum[32];
    char egl[32], gles[32];
    char egl_cache[256];
};
/* No libc: also called from the resident exec syscall path. */
static inline void maton_gpu_payload(struct maton_gpu_bootstrap *b,
                                     uintptr_t phdr, uintptr_t phnum) {
    char *fields[2]={b->phdr,b->phnum};
    uintptr_t values[2]={phdr,phnum};
    for(unsigned i=0;i<2;i++) {
        for(unsigned j=0;j<2*sizeof(uintptr_t);j++)
            fields[i][j]="0123456789abcdef"[(values[i]>>(4*(2*sizeof(uintptr_t)-1-j)))&15];
        fields[i][2*sizeof(uintptr_t)]=0;
    }
}
#endif
