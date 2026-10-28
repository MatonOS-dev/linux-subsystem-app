#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>
int main(int argc,char **argv) {
    if(sizeof(void *)==8) {
        FILE *maps=fopen("/proc/self/maps","r");if(!maps)return 1;
        char line[4096];int found=0;
        while(fgets(line,sizeof(line),maps))if(strstr(line,"730000000000-730000001000 ---p"))found=1;
        fclose(maps);if(!found){puts("FAIL inherited native hole missing");return 2;}
        void *address=(void *)(uintptr_t)UINT64_C(0x730000000000);
        errno=0;
        if(mmap(address,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=MAP_FAILED || errno!=EACCES)return 4;
        errno=0;
        if(munmap(address,4096)!=-1 || errno!=EACCES)return 5;
        errno=0;
        if(mprotect(address,4096,PROT_READ|PROT_EXEC)!=-1 || errno!=EACCES)return 6;
    }
    if(argc>1){execv(argv[1],argv+1);perror("native-state exec");return 3;}
    puts("PASS inherited native IP exemption remains reserved across ELF64 -> ELF32 -> ELF64 kernel exec");return 0;
}
