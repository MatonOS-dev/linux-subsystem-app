#include "../../proxy.h"
#include "sysinfo.h"
#include <sys/mman.h>
#include <sys/prctl.h>

__attribute__((visibility("hidden"))) uintptr_t proxy_i386_payload_sysinfo;

static int sysinfo_enabled;
static int sysinfo_configured;

void proxy_i386_sysinfo_configure(char *const *env) {
    if(!proxy_i386_payload_sysinfo) {
        /* A dynamic peer's entire DSO (including its gateway section) is
         * relocated into the exempt bionic region. Allocate the payload
         * fallback separately, outside all reserved native ranges. */
        long page=RAW(PROXY_SC_mmap,0,4096,PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if((unsigned long)page<(unsigned long)-4095) {
            const unsigned char code[]={0x9c,0xcd,0x80,0x9d,0xc3};
            p_copy((void *)page,code,sizeof(code)); /* pushfl; int80; popfl; ret */
            RAW(PROXY_SC_prctl,PR_SET_VMA,PR_SET_VMA_ANON_NAME,page,4096,
                "matonos-i386-sysinfo-trap",0);
            long error=RAW(PROXY_SC_mprotect,page,4096,PROT_READ|PROT_EXEC,0,0,0);
            if(!error)proxy_i386_payload_sysinfo=(uintptr_t)page;
            else RAW(PROXY_SC_munmap,page,4096,0,0,0,0);
        }
        if(!proxy_i386_payload_sysinfo)proxy_log("payload sysinfo allocation failed",0,-ENOMEM);
    }
    int enabled=0;
    for (size_t i=0; env && env[i]; i++) {
        if (p_equal(env[i], "MATON_PROXY_REWRITE=1")) {
            enabled=1;
            break;
        }
    }
    if (!sysinfo_configured || enabled!=sysinfo_enabled)
        proxy_log("i386 AT_SYSINFO fast path", enabled, 0);
    sysinfo_enabled=enabled;
    sysinfo_configured=1;
}

uintptr_t proxy_i386_sysinfo_value(uintptr_t inherited) {
    extern void proxy_i386_sysinfo_entry(void);

    /* The dynamic peer replaces native AT_SYSINFO with an exempt gateway
     * before entering bionic. Never lend that exemption to glibc: older
     * runtimes call AT_SYSINFO directly, bypassing every payload trap (not
     * just ioctl). Keep a register-preserving payload-only trap trampoline
     * when direct dispatch is disabled. A missing AT_SYSINFO stays missing. */
    return sysinfo_enabled && proxy_i386_payload_sysinfo ? (uintptr_t)proxy_i386_sysinfo_entry :
        inherited ? proxy_i386_payload_sysinfo : 0;
}

__attribute__((force_align_arg_pointer))
long proxy_i386_sysinfo_dispatch(long nr, proxy_arg_t *raw) {
    extern long proxy_direct_dispatch(long, proxy_arg_t *);
    proxy_args args; p_copy(args,raw,sizeof(args));
    if (nr == 140) { /* _llseek(fd, high, low, result*, whence) */
        uint64_t offset = ((uint64_t)(uint32_t)raw[1] << 32) | (uint32_t)raw[2];
        args[0] = (int32_t)raw[0]; args[1] = (int64_t)offset; args[2] = (int32_t)raw[4];
        long result = proxy_direct_dispatch(19, args);
        if (result < 0) return result;
        if (proxy_copy((void *)(uintptr_t)(uint32_t)raw[3], &result, sizeof(uint32_t)))
            return -EFAULT;
        return 0;
    }
    return proxy_direct_dispatch(nr, args);
}
