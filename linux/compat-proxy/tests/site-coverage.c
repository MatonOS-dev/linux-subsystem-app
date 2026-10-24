#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdio.h>
int main(void){
    const char *names[]={"openat","close","fstatat","__fxstatat","statx","fstat","readlink","readlinkat","getdents64","lseek","fcntl"};
    for(unsigned j=0;j<sizeof(names)/sizeof(*names);j++){
        void *fn=dlsym(RTLD_DEFAULT,names[j]);Dl_info info;ElfW(Sym) *symbol=NULL;
        if(!fn || !dladdr1(fn,&info,(void **)&symbol,RTLD_DL_SYMENT) || !symbol)continue;
        const unsigned char *p=fn;unsigned trapped=0,patched=0;
        for(size_t i=0;i+7<=symbol->st_size;i++)if(p[i]==0xb8){
            if(p[i+5]==0x0f && p[i+6]==5)trapped++;
            if(p[i+5]==0xff && p[i+6]==0xd0)patched++;
        }
        printf("coverage %-12s bytes=%lu primary_trapped=%u patched=%u\n",names[j],(unsigned long)symbol->st_size,trapped,patched);
    }
    puts("loading libstdc++.so.6");fflush(stdout);
    void *large=dlopen("libstdc++.so.6",RTLD_NOW|RTLD_LOCAL);assert(large);assert(!dlclose(large));
    for(unsigned i=0;i<3;i++){
        puts("loading site-rewrite.so");fflush(stdout);
        void *test=dlopen("./build/site-rewrite.so",RTLD_NOW|RTLD_LOCAL);assert(test);assert(!dlclose(test));
    }
    puts("reloading libstdc++.so.6");fflush(stdout);
    large=dlopen("libstdc++.so.6",RTLD_NOW|RTLD_LOCAL);assert(large);assert(!dlclose(large));
}
