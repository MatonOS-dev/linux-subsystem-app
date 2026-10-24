/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MATONOS_RESIDENT_RANGES_H
#define MATONOS_RESIDENT_RANGES_H

#include <stddef.h>
#include <stdint.h>
#include <errno.h>

#ifndef RESIDENT_ALLOC
#define RESIDENT_ALLOC malloc
#define RESIDENT_FREE free
#endif

static inline int resident_path_equal(const char *a, const char *b) {
    while (*a&&*b&&*a==*b) { a++; b++; }
    return !*a&&!*b;
}

static inline int resident_path_has_libc(const char *path) {
    for (;*path;path++)
        if (path[0]=='l'&&path[1]=='i'&&path[2]=='b'&&path[3]=='c'&&
            path[4]=='.'&&path[5]=='s'&&path[6]=='o') return 1;
    return 0;
}

struct arch_ip_range { uint64_t begin, end; unsigned linker; };

/* The host Android linker is part of the resident bionic runtime. Match its
 * known full map paths exactly; payload glibc/musl loaders never qualify. */
static inline int resident_linker_path(const char *path) {
    return resident_path_equal(path, "/apex/com.android.runtime/bin/linker") ||
           resident_path_equal(path, "/system/bin/linker") ||
           resident_path_equal(path, "/apex/com.android.runtime/bin/linker64") ||
           resident_path_equal(path, "/system/bin/linker64");
}

static inline int resident_libc_path(const char *path) {
    return resident_path_has_libc(path);
}

static inline int resident_bionic_path(const char *path) {
    return resident_libc_path(path) || resident_linker_path(path);
}

/* Android-side system libraries execute inside the stub process too. Their
 * syscalls retain native Android semantics; Linux payloads stay outside these
 * mapped system/vendor/APEX paths. */
static inline int resident_android_library_path(const char *path) {
    static const char *const prefixes[]={
        "/system/lib/", "/system/lib64/",
        "/vendor/lib/", "/vendor/lib64/",
        "/apex/"
    };
    for (size_t i=0;i<sizeof(prefixes)/sizeof(prefixes[0]);i++) {
        const char *a=path,*b=prefixes[i];
        while (*b&&*a==*b) { a++;b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* Only Android graphics driver and HAL mappings need native syscall
 * semantics while a Linux thread temporarily borrows bionic TLS. */
static inline int resident_android_graphics_path(const char *path) {
    static const char *const prefixes[]={
        "/vendor/lib/hw/", "/vendor/lib64/hw/",
        "/vendor/lib/egl/", "/vendor/lib64/egl/",
        "/system/lib/hw/", "/system/lib64/hw/",
        "/system/lib/egl/", "/system/lib64/egl/"
    };
    for (size_t i=0;i<sizeof(prefixes)/sizeof(prefixes[0]);i++) {
        const char *a=path,*b=prefixes[i];
        while (*b&&*a==*b) { a++;b++; }
        if (!*b) return 1;
    }
    static const char *const names[]={
        "libEGL", "libGLES", "libvulkan", "libgralloc", "libminigbm",
        "libgallium", "libdrm", "libgbm", "vulkan.", "egl_", "gralloc."
    };
    const char *base=path;
    for (const char *p=path;*p;p++) if (*p=='/') base=p+1;
    for (size_t i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        const char *a=base,*b=names[i];
        while (*a&&*a==*b) { a++;b++; }
        if (!*b) return 1;
    }
    return 0;
}

static inline int resident_range_append(struct arch_ip_range **ranges,
                                        size_t *count, size_t *capacity,
                                        uintptr_t begin, uintptr_t end,
                                        unsigned linker) {
    if (begin >= end) return 0;
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 8;
        if (next < *capacity || next > SIZE_MAX / sizeof(**ranges)) return -1;
        void *grown = RESIDENT_ALLOC(next * sizeof(**ranges));
        if (!grown) return -1;
        for (size_t i=0;i<*count;i++) ((struct arch_ip_range *)grown)[i]=(*ranges)[i];
        RESIDENT_FREE(*ranges);
        *ranges = grown;
        *capacity = next;
    }
    (*ranges)[(*count)++] = (struct arch_ip_range){begin, end, linker};
    return 0;
}

static inline uintptr_t resident_maps_hex(const char **cursor) {
    uintptr_t value=0;
    for (;;) {
        unsigned c=(unsigned char)**cursor, digit;
        if (c>='0'&&c<='9') digit=c-'0';
        else if (c>='a'&&c<='f') digit=c-'a'+10;
        else break;
        value=(value<<4)|digit;
        (*cursor)++;
    }
    return value;
}

/* Parses a complete /proc/self/maps buffer in place. */
static inline int resident_ranges_parse(char *maps, size_t *count,
                                        struct arch_ip_range **ranges,
                                        size_t *capacity) {
    for (char *line=maps; *line;) {
        char *end=line;
        while (*end&&*end!='\n') end++;
        char saved=*end; *end=0;
        const char *p=line;
        uintptr_t lo=resident_maps_hex(&p), hi=0;
        if (*p=='-') { p++; hi=resident_maps_hex(&p); }
        while (*p==' ') p++;
        int executable=p[0]=='r'&&p[1]=='-'&&p[2]=='x';
        while (*p&&*p!=' ') p++;
        while (*p==' ') p++;
        /* Skip file offset, device, and inode to reach the full map pathname. */
        for (unsigned field=0;field<3&&*p;field++) {
            while (*p&&*p!=' ') p++;
            while (*p==' ') p++;
        }
        int linker=resident_linker_path(p);
        if (lo<hi&&executable&&(resident_bionic_path(p)||resident_android_library_path(p)) &&
            resident_range_append(ranges,count,capacity,lo,hi,(unsigned)linker)) {
            *end=saved;
            return -ENOMEM;
        }
        *end=saved;
        line=saved?end+1:end;
    }
    return 0;
}

#endif
