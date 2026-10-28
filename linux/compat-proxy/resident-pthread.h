/* SPDX-License-Identifier: Apache-2.0
 * Resident libc threads must never steal a payload signalfd's SIGCHLD.
 * Interpose the public pthread entry point, so lazy EGL/Vulkan/Binder workers
 * have the same mask as workers created during loader warmup. Adopted payload
 * threads explicitly restore the Linux caller's mask in their start gate.
 * Resolve only the next resident-libc symbol; payload libc lives separately.
 */
#ifndef MATON_RESIDENT_PTHREAD_H
#define MATON_RESIDENT_PTHREAD_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <dlfcn.h>
#include <errno.h>
extern int proxy_resident_signals_begin(uint64_t *);
extern int proxy_resident_signals_end(uint64_t);
typedef int (*resident_pthread_create_fn)(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
static _Atomic(resident_pthread_create_fn) resident_pthread_create_next;
int pthread_create(pthread_t *thread,const pthread_attr_t *attr,void *(*start)(void *),void *arg) {
    resident_pthread_create_fn next=atomic_load_explicit(&resident_pthread_create_next,memory_order_acquire);
    if(!next){
        next=(resident_pthread_create_fn)dlsym(RTLD_NEXT,"pthread_create");
        if(!next)return ENOSYS;
        atomic_store_explicit(&resident_pthread_create_next,next,memory_order_release);
    }
    uint64_t previous;int error=proxy_resident_signals_begin(&previous);
    if(error)return -error;
    int result=next(thread,attr,start,arg);
    /* Valid kernel mask arguments cannot fail here. A failure after creating
     * a thread must not be reported as a failed pthread_create. */
    if(proxy_resident_signals_end(previous))__builtin_trap();
    return result;
}
#endif
