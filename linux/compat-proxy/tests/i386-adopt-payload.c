/* Compile with the NDK headers, link against compat glibc as i386-dynamic.sh
 * does. Only public pthread APIs and the published adoption v2 API are used. */
#include "../arch/i386/adopt.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/futex.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
/* This payload links glibc; the NDK header spells its public errno accessor differently. */
#undef errno
extern int *__errno_location(void);
#define errno (*__errno_location())
static struct adopt_api *api;
static __thread unsigned value;
static unsigned short expected_cw;
static struct user_desc extra_tls[2];
static unsigned extra_count;
static struct {struct robust_list_head head;struct robust_list node;unsigned word;} robust[7];
static void *worker(void *arg) {
    value=(unsigned)(uintptr_t)arg;
    unsigned short cw;__asm__ volatile("fnstcw %0":"=m"(cw));
    if(cw!=expected_cw)return (void *)4;
    for(unsigned i=0;i<100;i++) {
        struct adopt_scope outer,inner;
        long tid=syscall(SYS_gettid);uintptr_t native=0;
        if(!api->current())return (void *)11;
        int error=api->switch_in(&outer);if(error)return (void *)(uintptr_t)(20-error);
        error=api->switch_in(&inner);if(error)return (void *)(uintptr_t)(50-error);
        if(api->identity(&native)!=tid || !native)return (void *)2;
        api->switch_out(&inner);api->switch_out(&outer);
        if(value!=(unsigned)(uintptr_t)arg)return (void *)3;
    }
    for(unsigned i=0;i<extra_count;i++) {
        struct user_desc d={.entry_number=extra_tls[i].entry_number};
        if(syscall(SYS_get_thread_area,&d) || d.base_addr!=extra_tls[i].base_addr)return (void *)6;
    }
    if(value>=1 && value<=6) {
        unsigned slot=value;
        robust[slot].head.list.next=&robust[slot].node;
        robust[slot].head.futex_offset=sizeof(struct robust_list);
        robust[slot].node.next=&robust[slot].head.list;
        robust[slot].word=(unsigned)syscall(SYS_gettid)|FUTEX_WAITERS;
        if(syscall(SYS_set_robust_list,&robust[slot].head,sizeof(struct robust_list_head)))return (void *)5;
    }
    return NULL;
}
int main(void) {
    const char *address=getenv("MATON_THREAD_ADOPT");
    api=address?(void *)(uintptr_t)strtoul(address,NULL,16):NULL;
    __asm__ volatile("fnstcw %0":"=m"(expected_cw));
    expected_cw=(expected_cw&~0xc00u)|0x800u;
    __asm__ volatile("fldcw %0"::"m"(expected_cw));
    if(!api || api->version!=2 || worker((void *)123))return 1;
    /* Keep all three virtual TLS slots occupied while creating children.
     * The child must discard inherited hardware descriptors before adopting. */
    for(unsigned i=0;i<2;i++) {
        static unsigned words[2];
        struct user_desc d={.entry_number=(unsigned)-1,.base_addr=(unsigned)&words[i],
            .limit=0xfffff,.seg_32bit=1,.limit_in_pages=1,.useable=1};
        if(syscall(SYS_set_thread_area,&d))return 9;
        extra_tls[extra_count++]=d;
    }
    pthread_t threads[6];
    for(unsigned i=0;i<6;i++)if(pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)(i+1)))return 2;
    for(unsigned i=0;i<6;i++) {void *result=NULL;if(pthread_join(threads[i],&result)||result)return 3;}
    for(unsigned i=1;i<=6;i++)if(!(robust[i].word&FUTEX_OWNER_DIED))return 6;
    int pid=fork(),status=0;if(pid<0)return 7;
    if(!pid) {
        if(worker((void *)123))_exit(11);
        pthread_t nested;void *result=NULL;
        if(pthread_create(&nested,NULL,worker,(void *)123) || pthread_join(nested,&result) || result)_exit(12);
        _exit(0);
    }
    if(waitpid(pid,&status,0)!=pid || status){printf("fork child status=%#x\n",status);return 8;}
    /* Reject a fixed ELF mapping over the live, ASLR-relocated resident. */
    unsigned char image[4096]={0};
    Elf32_Ehdr *eh=(void *)image;Elf32_Phdr *ph=(void *)(image+sizeof(*eh));
    memcpy(eh->e_ident,ELFMAG,SELFMAG);eh->e_ident[EI_CLASS]=ELFCLASS32;
    eh->e_ident[EI_DATA]=ELFDATA2LSB;eh->e_ident[EI_VERSION]=EV_CURRENT;
    eh->e_type=ET_EXEC;eh->e_machine=EM_386;eh->e_version=EV_CURRENT;
    eh->e_ehsize=sizeof(*eh);eh->e_phoff=sizeof(*eh);eh->e_phentsize=sizeof(*ph);eh->e_phnum=1;
    uintptr_t base=(uintptr_t)api->current&~4095u;
    eh->e_entry=base;ph->p_type=PT_LOAD;ph->p_vaddr=base;
    ph->p_filesz=ph->p_memsz=sizeof(image);ph->p_flags=PF_R|PF_X;ph->p_align=4096;
    char path[512];snprintf(path,sizeof(path),"%s/resident-collision",getenv("TMPDIR"));
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0700);
    if(fd<0 || write(fd,image,sizeof(image))!=sizeof(image) || close(fd))return 10;
    char *args[]={path,NULL};extern char **environ;
    errno=0;if(execve(path,args,environ)!=-1 || errno!=ENOEXEC)return 11;
    unlink(path);
    puts("PASS i386 bionic adoption: main + six glibc pthreads, nested GS scopes, identity, join, three TLS slots, FP control, robust owner death, fork, resident collision");
    return 0;
}
