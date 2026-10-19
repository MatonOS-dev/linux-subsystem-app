#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include "../resident-ranges.h"
#include <stdio.h>

int main(void) {
    size_t maps_capacity=180000, used=0, count=0, ranges_capacity=0;
    char *maps=malloc(maps_capacity);
    struct arch_ip_range *ranges=NULL;
    if(!maps)return 1;
    for(unsigned i=0;i<900;i++) {
        int n=snprintf(maps+used,maps_capacity-used,
                       "%08x-%08x r-xp 00000000 00:00 0 /apex/com.android.runtime/lib64/bionic/libc.so\n",
                       0x100000+i*0x1000,0x100800+i*0x1000);
        if(n<0||(size_t)n>=maps_capacity-used)return 2;
        used+=(size_t)n;
    }
    const char *extra=
        "70000000-70010000 r-xp 00000000 00:00 0 /apex/com.android.runtime/bin/linker64\n"
        "71000000-71010000 r-xp 00000000 00:00 0 /system/bin/linker64.backup\n"
        "72000000-72010000 r-xp 00000000 00:00 0 /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2\n";
    size_t extra_len=strlen(extra);
    if(extra_len>=maps_capacity-used)return 3;
    memcpy(maps+used,extra,extra_len+1);
    used+=extra_len;
    if(used<=65536)return 4;
    if(resident_ranges_parse(maps,&count,&ranges,&ranges_capacity))return 5;
    if(count!=901){fprintf(stderr,"expected 901 resident ranges, got %zu from %zu bytes\n",count,used);return 6;}
    int found_linker=0;
    for(size_t i=0;i<count;i++) {
        if(ranges[i].begin==0x70000000&&ranges[i].end==0x70010000&&ranges[i].linker)found_linker=1;
        if(ranges[i].begin>=0x71000000)return 7;
    }
    if(!found_linker)return 8;
    free(ranges);free(maps);
    puts("PASS resident bionic ranges grow beyond old maps and range caps; payload loaders excluded");
    return 0;
}
