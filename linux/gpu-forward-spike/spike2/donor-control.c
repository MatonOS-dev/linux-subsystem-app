#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "provider.h"
#include "switch.h"
/* Bionic-native source control: demonstrate the gap between valid donor TLS
 * and actual thread adoption. Do not rewrite private pthread_internal_t fields. */
int main(void) {
    gs_init(0);
    shadow_publish();
    struct shadow_provider *p = (void *)(uintptr_t)strtoull(getenv("MATON_GPU_SHADOW"),NULL,16);
    struct shadow_lease lease;
    assert(!p->create(&lease));
    uintptr_t original = gs_read();
    long kernel_tid = gs_raw(186,0,0,0);
    gs_write(lease.fs);
    long libc_tid = gettid();
    gs_write(original);
    p->destroy(&lease);
    printf("kernel tid=%ld, borrowed bionic gettid=%ld; %s\n",kernel_tid,libc_tid,
           kernel_tid == libc_tid ? "no cached-tid mismatch observed" : "NOT ADOPTED: donor identity retained");
    return kernel_tid == libc_tid ? 1 : 0;
}
