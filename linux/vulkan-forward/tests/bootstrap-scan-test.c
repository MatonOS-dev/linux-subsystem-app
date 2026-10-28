/* Count backend reads deterministically; exercise every input chunk boundary. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../runtime.h"
#include "../arch/x86_64/backend.h"
static char contents[131072];
static size_t length,offset,chunk,reads;
static long scan_open(const char *path) {assert(!strcmp(path,"/proc/self/maps"));offset=0;return 42;}
static void scan_close(long fd) {assert(fd==42);}
static long scan_read(long fd,void *buffer,size_t size) {
    assert(fd==42);reads++;assert(size==4096);
    if(size>chunk)size=chunk;
    if(size>length-offset)size=length-offset;
    memcpy(buffer,contents+offset,size);offset+=size;return (long)size;
}
#define mv_open scan_open
#define mv_read scan_read
#define mv_close scan_close
#include "../runtime.c"
int main(void) {
    struct maton_gpu_bootstrap bootstrap={.magic=MATON_GPU_BOOTSTRAP_MAGIC,.bridge="1234",.adopt="abcd"};
    length=0;
    while(length<65536)length+=(size_t)sprintf(contents+length,"1234-5678 r--p 00000000 00:00 0 /unrelated/mapping\n");
    size_t prefix=length;
    length+=(size_t)sprintf(contents+length,"%lx-%lx r--p 00000000 00:00 0 [anon:maton-gpu-bootstrap-v2]\n",
        (unsigned long)&bootstrap,(unsigned long)&bootstrap+4096);
    for(chunk=1;chunk<=4096;chunk=chunk==1?17:chunk==17?4095:chunk+1) {
        reads=0;cached_bootstrap=NULL;
        assert(!strcmp(mv_env("MATON_GPU_BRIDGE"),"1234"));
        assert(!strcmp(mv_env("MATON_THREAD_ADOPT"),"abcd"));
        assert(reads<=(length+chunk-1)/chunk);
        if(chunk==4096)assert(reads<32);
    }
    length=prefix;chunk=4096;reads=0;cached_bootstrap=NULL;
    assert(!mv_env("MATON_GPU_BRIDGE"));assert(reads<32);
    /* A miss must not poison discovery if a resident publishes later. */
    length+=(size_t)sprintf(contents+length,"%lx-%lx r--p 00000000 00:00 0 [anon:maton-gpu-bootstrap-v2]\n",
        (unsigned long)&bootstrap,(unsigned long)&bootstrap+4096);
    assert(!strcmp(mv_env("MATON_GPU_BRIDGE"),"1234"));
    puts("PASS bootstrap scan: chunk boundaries, bounded reads, fresh discovery after miss");
}
