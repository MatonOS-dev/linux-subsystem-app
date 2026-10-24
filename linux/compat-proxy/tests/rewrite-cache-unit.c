#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../proxy.c"
void *vfs_alloc(size_t n){return malloc(n);}
void vfs_free(void *p){free(p);}
void p_copy(void *out,const void *in,size_t n){memcpy(out,in,n);}
int main(void){
    unsigned char original[]={0xb8,1,1,0,0,0x49,0x89,0xca,15,5,0xc3};
    unsigned char live[sizeof(original)];memcpy(live,original,sizeof(live));
    struct rewrite_record record={.address=(uintptr_t)live,.window=10,.active=1,.nr=257};
    memcpy(record.original,original,10);rewrite_records=&record;
    arch_rewrite_table=0x40000000;arch_rewrite_patch(live,257,10);
    assert(rewrite_function_crc(live,sizeof(live))==rewrite_crc(original,sizeof(original)));
    /* An app changes an intervening instruction in a previously patched window.
     * Normalization must not hide that change from the proof checksum. */
    live[6]^=1;
    assert(rewrite_function_crc(live,sizeof(live))!=rewrite_crc(original,sizeof(original)));
    puts("PASS patch normalization preserves external changes in the function checksum");
}
