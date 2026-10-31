#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#define _start unused_peer_start_symbol
#define peer_start unused_peer_start_function
#include "../arch/x86_64/peer-start.c"
long peer_raw(long n,long a,long b,long c,long d,long e,long f){
    long r=syscall(n,a,b,c,d,e,f);return r==-1?-errno:r;
}
int main(void){
    /* Keep storage off the brk heap while constructing the hidden-name VMA. */
    temporary_capacity=4;
    temporary_ranges=mapping(4096);
    uintptr_t lo=(uintptr_t)syscall(SYS_brk,0);
    assert(!(lo&4095));
    assert((uintptr_t)syscall(SYS_brk,lo+8192)==lo+8192);
    assert(!peer_raw(11,lo,8192,0,0,0,0));
    reserve(lo,lo+4096,1);
    int hidden=0;
    for(char *p=read_maps();*p;){
        uintptr_t begin=hex(&p);assert(*p++=='-');uintptr_t end=hex(&p);
        char *line=p;while(*p && *p!='\n')p++;
        char saved=*p;*p=0;
        if(begin<=lo && end>lo && strstr(line,"[heap]"))hidden=1;
        *p=saved;if(*p)p++;
    }
    assert(hidden);
    assert(peer_raw(9,lo,4096,0,MAP_PRIVATE|MAP_ANONYMOUS|NOREPLACE,-1,0)==-EEXIST);
    release_holes();
    assert(peer_raw(9,lo,4096,0,MAP_PRIVATE|MAP_ANONYMOUS|NOREPLACE,-1,0)==(long)lo);
    assert(!peer_raw(11,lo,4096,0,0,0,0));
    const char msg[]="PASS temporary native reservation displayed as heap is released by exact range\n";
    peer_raw(1,1,(long)msg,sizeof(msg)-1,0,0,0);
    peer_raw(231,0,0,0,0,0,0);
}
