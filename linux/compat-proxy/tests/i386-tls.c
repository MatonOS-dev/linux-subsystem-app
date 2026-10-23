/* Freestanding compile against the host's compat glibc, so no multilib
 * development headers/startup objects are needed. */
typedef unsigned int uint32_t;
typedef unsigned long intptr_t;
extern int pthread_create(unsigned long *,const void *,void *(*)(void *),void *);
extern int pthread_join(unsigned long,void **);
extern long syscall(long,...);
extern long write(int,const void *,unsigned long);
extern void _exit(int);
extern void (*signal(int,void (*)(int)))(int);
extern int raise(int);
#define TEST_SIGSEGV 11

/* Linux's 32-bit asm/ldt.h wire structure. Some hosts lack multilib headers. */
struct user_desc {
    unsigned int entry_number;
    unsigned int base_addr;
    unsigned int limit;
    unsigned int seg_32bit:1;
    unsigned int contents:2;
    unsigned int read_exec_only:1;
    unsigned int limit_in_pages:1;
    unsigned int seg_not_present:1;
    unsigned int useable:1;
};

static __thread volatile int thread_value;
static volatile int threads_ready;
static volatile int segv_chained;
static void app_segv(int sig) { if(sig==TEST_SIGSEGV)segv_chained++; }

static void check(int ok, const char *what) {
    if (!ok) {
        unsigned long n=0;while(what[n])n++;
        write(2,what,n);write(2,"\n",1);_exit(1);
    }
}

static void exercise_ldt_cache(unsigned first, int expected) {
    for(unsigned i=0;i<5;i++) {
        struct user_desc d={.entry_number=first+i,.base_addr=(unsigned)&thread_value,
            .limit=0xfffff,.seg_32bit=1,.contents=0,.read_exec_only=0,
            .limit_in_pages=1,.seg_not_present=0,.useable=1};
        check(syscall(123,1,&d,sizeof(d))==0,"write concurrent LDT selector");
        unsigned short selector=(unsigned short)(((first+i)<<3)|7u),old_fs;
        int loaded=0;
        __asm__ volatile("mov %%fs,%0\n\t"
                         "mov %2,%%fs\n\t"
                         "movl %%fs:0,%1\n\t"
                         "mov %0,%%fs"
                         :"=&r"(old_fs),"=&r"(loaded):"rm"(selector):"memory");
        check(loaded==expected,"LDT selector cache and eviction");
    }
}

static void *thread_main(void *arg) {
    int value=(int)(intptr_t)arg;
    thread_value=value;
    __sync_fetch_and_add(&threads_ready,1);
    while(threads_ready<4)__asm__ volatile("pause");
    check(thread_value==value,"pthread __thread value isolation");
    exercise_ldt_cache(32u+(unsigned)value*8u,value);
    return (void *)(intptr_t)(thread_value==value?0:1);
}

int main(void) {
    unsigned long threads[4];
    for (int i=0;i<4;i++)check(pthread_create(&threads[i],(void *)0,thread_main,(void *)(intptr_t)(i+11))==0,"pthread_create");
    for (int i=0;i<4;i++) {
        void *result=(void *)0;
        check(pthread_join(threads[i],&result)==0 && result==0,"pthread_join / TLS isolation");
    }

    uint32_t value=0x51a7c0de;
    struct user_desc desc={.entry_number=-1,.base_addr=(unsigned long)&value,
        .limit=0xfffff,.seg_32bit=1,.contents=0,.read_exec_only=0,
        .limit_in_pages=1,.seg_not_present=0,.useable=1};
    check(syscall(243,&desc)==0,"set_thread_area");
    check(desc.entry_number!=0xffffffffU,"set_thread_area allocated entry");
    struct user_desc fetched={.entry_number=desc.entry_number};
    check(syscall(244,&fetched)==0,"get_thread_area");
    check(fetched.base_addr==desc.base_addr && fetched.limit==desc.limit &&
          fetched.seg_32bit==desc.seg_32bit,"get_thread_area descriptor");

    unsigned short old_gs,selector=(unsigned short)((desc.entry_number<<3)|3);
    __asm__ volatile("mov %%gs,%0":"=rm"(old_gs));
    __asm__ volatile("mov %0,%%gs"::"rm"(selector):"memory");
    uint32_t loaded;
    __asm__ volatile("movl %%gs:0,%0":"=r"(loaded));
    __asm__ volatile("mov %0,%%gs"::"rm"(old_gs):"memory");
    check(loaded==value,"GS selector reads allocated TLS entry");
    struct user_desc ldt={.entry_number=1,.base_addr=(unsigned long)&value,
        .limit=0xfffff,.seg_32bit=1,.contents=0,.read_exec_only=0,
        .limit_in_pages=1,.seg_not_present=0,.useable=1};
    check(syscall(123,1,&ldt,sizeof(ldt))==0,"modify_ldt data write");
    unsigned char raw[16]={0};
    check(syscall(123,0,raw,sizeof(raw))==sizeof(raw),"modify_ldt read");
    check(raw[8]!=0 || raw[9]!=0 || raw[10]!=0 || raw[11]!=0,
          "modify_ldt read returns stored descriptor");
    check(signal(TEST_SIGSEGV,app_segv)!=((void (*)(int))-1),"install app SIGSEGV handler");
    check(raise(TEST_SIGSEGV)==0 && segv_chained==1,"proxy chains non-LDT SIGSEGV to app handler");
    unsigned short old_fs,ldt_selector=(1u<<3)|7u;
    uint32_t ldt_loaded=0;
    __asm__ volatile("mov %%fs,%0\n\t"
                     "movw %2,%%fs\n\t"
                     "movl %%fs:0,%1\n\t"
                     "mov %0,%%fs"
                     :"=r"(old_fs),"=r"(ldt_loaded)
                     :"m"(ldt_selector):"memory");
    check(ldt_loaded==value,"LDT FS selector remapped and reads descriptor base");
    struct { uint32_t offset; unsigned short selector; } farptr={0,(unsigned short)ldt_selector};
    unsigned short old_seg;uint32_t far_value=0;
    __asm__ volatile("mov %%es,%0\n\tles %3,%1\n\tmovl %%es:0,%2\n\tmov %0,%%es"
                     :"=&r"(old_seg),"=&r"(loaded),"=r"(far_value):"m"(farptr):"memory");
    check(far_value==value,"LES loads an LDT selector and reads its nonzero base");
    __asm__ volatile("mov %%ds,%0\n\tlds %3,%1\n\tmovl %%ds:0,%2\n\tmov %0,%%ds"
                     :"=&r"(old_seg),"=&r"(loaded),"=r"(far_value):"m"(farptr):"memory");
    check(far_value==value,"LDS loads an LDT selector and reads its nonzero base");
    /* LSS changes SS and the selected GPR together. Use a zero-base
     * descriptor so the stack remains addressable during the brief load. */
    struct user_desc stackdesc={.entry_number=6,.base_addr=0,.limit=0xfffff,
        .seg_32bit=1,.contents=0,.read_exec_only=0,.limit_in_pages=1,
        .seg_not_present=0,.useable=1};
    check(syscall(123,1,&stackdesc,sizeof(stackdesc))==0,"write zero-base LSS descriptor");
    struct { uint32_t offset; unsigned short selector; } stackptr={0,(unsigned short)((6u<<3)|7u)};
    __asm__ volatile("mov %%ss,%0\n\tlss %1,%%eax\n\tmov %0,%%ss"
                     :"=&r"(old_seg):"m"(stackptr):"eax","memory");
    unsigned pop_selector=ldt_selector;
    unsigned short original_es,current_es;
    __asm__ volatile("mov %%es,%0":"=r"(original_es));
    __asm__ volatile("pushl %0\n\tpop %%es"::"r"(pop_selector):"memory");
    __asm__ volatile("mov %%es,%0":"=r"(current_es));
    check((current_es&4)!=0,"POP ES accepts an emulated LDT selector");
    __asm__ volatile("mov %0,%%es"::"rm"(original_es):"memory");
    ldt.contents=2;
    check(syscall(123,1,&ldt,sizeof(ldt))==-1,"modify_ldt rejects code descriptors");
    ldt.contents=0;ldt.seg_32bit=0;
    check(syscall(123,1,&ldt,sizeof(ldt))==-1,"modify_ldt rejects 16-bit descriptors");
    static const char pass[]="PASS i386 pthread TLS, multi-selector LDT cache, far segment loads, and SIGSEGV chaining\n";
    write(1,pass,sizeof(pass)-1);
    return 0;
}
