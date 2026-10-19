/* Binder ioctls are emulated/routed like other payload calls and reach Linux. */
#include <assert.h>
#include <errno.h>
#include <linux/android/binder.h>
#include <linux/dma-buf.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* Inline syscall executes in this payload's text; ioctl() below enters libc. */
static long payload_ioctl(unsigned long request) {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(SYS_ioctl), "D"(-1), "S"(request), "d"(0) : "rcx", "r11", "memory");
    return result;
}

int main(void) {
    unsigned long binder[]={BINDER_WRITE_READ,BINDER_SET_CONTEXT_MGR,BINDER_VERSION};
    unsigned long graphics[]={DMA_BUF_IOCTL_SYNC,DMA_BUF_IOCTL_EXPORT_SYNC_FILE,DMA_BUF_IOCTL_IMPORT_SYNC_FILE};
    for(unsigned i=0;i<sizeof(binder)/sizeof(binder[0]);i++) {
        errno=0;assert(ioctl(-1,binder[i],0)==-1 && errno==EBADF);
        assert(payload_ioctl(binder[i])==-EBADF);
    }
    for(unsigned i=0;i<sizeof(graphics)/sizeof(graphics[0]);i++) {
        errno=0;assert(ioctl(-1,graphics[i],0)==-1 && errno==EBADF);
    }
    pid_t child=fork();assert(child>=0);
    if(!child){syscall(0x40000000|SYS_getpid);_exit(1);}
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGSYS);
    puts("PASS Binder ioctls reach kernel, DMA-BUF sync/fence passthrough and x32 rejection");
    return 0;
}
