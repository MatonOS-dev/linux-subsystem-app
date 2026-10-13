/* Kernel-filter denial survives translated ioctls, while GPU fence ioctls pass. */
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

int main(void) {
    unsigned long blocked[]={BINDER_WRITE_READ,BINDER_SET_CONTEXT_MGR,BINDER_VERSION};
    unsigned long graphics[]={DMA_BUF_IOCTL_SYNC,DMA_BUF_IOCTL_EXPORT_SYNC_FILE,DMA_BUF_IOCTL_IMPORT_SYNC_FILE};
    for(unsigned i=0;i<sizeof(blocked)/sizeof(blocked[0]);i++) {
        errno=0;assert(ioctl(-1,blocked[i],0)==-1 && errno==EPERM);
    }
    for(unsigned i=0;i<sizeof(graphics)/sizeof(graphics[0]);i++) {
        errno=0;assert(ioctl(-1,graphics[i],0)==-1 && errno==EBADF);
    }
    pid_t child=fork();assert(child>=0);
    if(!child){syscall(0x40000000|SYS_getpid);_exit(1);}
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGSYS);
    puts("PASS kernel Binder denial, DMA-BUF sync/fence passthrough and x32 rejection");
    return 0;
}
