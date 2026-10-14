/* The boundary must round-trip every supported number and reject x32. */
#include "../arch/arch.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    for(long id=0;id<PROXY_SC_count;id++) {
        long nr=arch_encode_syscall(id);
        if(nr>=0) assert(arch_decode_syscall(nr)==id);
    }
    assert(arch_decode_syscall(-1)==PROXY_SC_unknown);
    assert(arch_decode_syscall(0x40000000)==PROXY_SC_unknown);
    assert(arch_encode_syscall(PROXY_SC_unknown)==-1);
    puts("PASS architecture syscall IDs round-trip and reject unknown/x32");
}
