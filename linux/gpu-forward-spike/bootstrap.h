#ifndef MATON_GPU_BOOTSTRAP_H
#define MATON_GPU_BOOTSTRAP_H
#include <stdint.h>
/* Read-only named mapping; no fixed virtual address, fd, libc or TCB ABI. */
#define MATON_GPU_BOOTSTRAP_MAGIC UINT64_C(0x4d41544f4e475031)
#define MATON_GPU_BOOTSTRAP_NAME "maton-gpu-bootstrap"
struct maton_gpu_bootstrap {
    uint64_t magic;
    char bridge[32], adopt[32], fast[32];
    /* Optional Linux Wayland WSI helper vtable address (hex), or "0". */
    char wsi[32];
};
#endif
