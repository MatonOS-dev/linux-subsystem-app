#include "../../proxy.h"
#include "sysinfo.h"

static int sysinfo_enabled;
static int sysinfo_configured;

void proxy_i386_sysinfo_configure(char *const *env) {
    int enabled=1;
    for (size_t i=0; env && env[i]; i++) {
        if (p_equal(env[i], "MATON_PROXY_REWRITE=0")) {
            enabled=0;
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
    return sysinfo_enabled ? (uintptr_t)proxy_i386_sysinfo_entry : inherited;
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
