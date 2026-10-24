#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
static void *exercise(void *unused){
    (void)unused;struct stat st;char buf[4096];
    int fd=openat(AT_FDCWD,"/etc/hosts",O_RDONLY);assert(fd>=0);
    assert(!fstat(fd,&st));assert(!fstatat(AT_FDCWD,"/etc/hosts",&st,0));
    (void)lseek(fd,0,SEEK_END);int dup=fcntl(fd,F_DUPFD,100);assert(dup>=0);close(dup);struct flock lock={.l_type=F_UNLCK,.l_whence=SEEK_SET};assert(!fcntl(fd,F_SETLKW,&lock));close(fd);
    assert(readlink("/proc/self/exe",buf,sizeof(buf))>0);assert(readlinkat(AT_FDCWD,"/proc/self/exe",buf,sizeof(buf))>0);
    int (*sx)(int,const char *,int,unsigned,void *)=dlsym(RTLD_DEFAULT,"statx");if(sx)assert(!sx(AT_FDCWD,"/etc/hosts",0,0x7ff,buf));
    ssize_t (*dents)(int,void *,size_t)=dlsym(RTLD_DEFAULT,"getdents64");if(dents){fd=openat(AT_FDCWD,"/tmp",O_RDONLY|O_DIRECTORY);assert(fd>=0);assert(dents(fd,buf,sizeof(buf))>=0);close(fd);}
    return NULL;
}
static size_t symbol_size(const char *file,uintptr_t value){
    FILE *f=fopen(file,"rb");if(!f)return 0;Elf64_Ehdr eh;size_t size=0;
    if(fread(&eh,1,sizeof(eh),f)!=sizeof(eh))goto done;
    for(unsigned i=0;i<eh.e_shnum;i++){
        Elf64_Shdr sh;if(fseek(f,eh.e_shoff+i*sizeof(sh),SEEK_SET) || fread(&sh,1,sizeof(sh),f)!=sizeof(sh))break;
        if(sh.sh_type!=SHT_DYNSYM)continue;
        for(size_t j=0;j<sh.sh_size/sizeof(Elf64_Sym);j++){
            Elf64_Sym sym;if(fseek(f,sh.sh_offset+j*sizeof(sym),SEEK_SET) || fread(&sym,1,sizeof(sym),f)!=sizeof(sym))break;
            if(sym.st_value==value && ELF64_ST_TYPE(sym.st_info)==STT_FUNC){size=sym.st_size;goto done;}
        }
    }
 done:fclose(f);return size;
}
int main(void){
    exercise(NULL);pthread_t t;assert(!pthread_create(&t,NULL,exercise,NULL));assert(!pthread_join(t,NULL));
    const char *names[]={"openat","close","fstatat","statx","fstat","readlink","readlinkat","getdents64","lseek","fcntl"};
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++){
        const unsigned char *p=dlsym(RTLD_DEFAULT,names[i]);Dl_info info;
        if(!p || !dladdr(p,&info))continue;
        size_t n=symbol_size(info.dli_fname,(uintptr_t)p-(uintptr_t)info.dli_fbase);unsigned raw=0,patched=0;
        for(size_t j=0;j+1<n;j++){if(p[j]==15 && p[j+1]==5)raw++;if(p[j]==255 && p[j+1]==208)patched++;}
        printf("%-12s bytes=%zu syscall_bytes=%u call_rax_bytes=%u\n",names[i],n,raw,patched);
    }
}
