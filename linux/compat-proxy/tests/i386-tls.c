/* Freestanding compile against the host's compat glibc, so no multilib
 * development headers/startup objects are needed. */
typedef unsigned int uint32_t;
typedef unsigned long intptr_t;
extern int pthread_create(unsigned long *,const void *,void *(*)(void *),void *);
extern int pthread_join(unsigned long,void **);
extern long syscall(long,...);
extern long write(int,const void *,unsigned long);
extern void _exit(int);

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

static void check(int ok, const char *what) {
    if (!ok) {
        unsigned long n=0;while(what[n])n++;
        write(2,what,n);write(2,"\n",1);_exit(1);
    }
}

static void *thread_main(void *arg) {
    int value=(int)(intptr_t)arg;
    thread_value=value;
    __sync_fetch_and_add(&threads_ready,1);
    while(threads_ready<4)__asm__ volatile("pause");
    check(thread_value==value,"pthread __thread value isolation");
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
    static const char pass[]="PASS i386 pthread TLS, set/get_thread_area, and GS selector read\n";
    write(1,pass,sizeof(pass)-1);
    return 0;
}
