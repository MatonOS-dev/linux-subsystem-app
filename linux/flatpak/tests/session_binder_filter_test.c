/* Actual kernel seccomp check: Binder transaction/control commands return EPERM;
 * allowed DMA-BUF commands reach the kernel and return EBADF for fd -1. */
#include "../session-binder-filter.h"
#include <linux/android/binder.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void) {
    const unsigned long binder[]={BINDER_WRITE_READ,
        BINDER_SET_MAX_THREADS,BINDER_SET_IDLE_PRIORITY,BINDER_SET_CONTEXT_MGR,
        BINDER_THREAD_EXIT,BINDER_VERSION,BINDER_GET_NODE_DEBUG_INFO,
        BINDER_GET_NODE_INFO_FOR_REF,BINDER_SET_CONTEXT_MGR_EXT,BINDER_FREEZE,
        BINDER_GET_FROZEN_INFO,BINDER_ENABLE_ONEWAY_SPAM_DETECTION,
        BINDER_GET_EXTENDED_ERROR,
        /* 32-bit binder_write_read encoding must remain blocked too. */
        _IOC(_IOC_READ|_IOC_WRITE,'b',1,24)};
    const unsigned long dma[]={DMA_BUF_IOCTL_SYNC,DMA_BUF_IOCTL_EXPORT_SYNC_FILE,
        DMA_BUF_IOCTL_IMPORT_SYNC_FILE,
        /* Same encoding as DMA-BUF import: this setting does not transact. */
        BINDER_SET_IDLE_TIMEOUT};
    if(session_filter_binder()){perror("install filter");return 1;}
    for(size_t i=0;i<sizeof(binder)/sizeof(binder[0]);i++) {
        errno=0;
        if(ioctl(-1,binder[i],NULL)!=-1 || errno!=EPERM) {
            fprintf(stderr,"Binder %#lx was not blocked: errno=%d\n",binder[i],errno);return 1;
        }
    }
    for(size_t i=0;i<sizeof(dma)/sizeof(dma[0]);i++) {
        errno=0;
        if(ioctl(-1,dma[i],NULL)!=-1 || errno!=EBADF) {
            fprintf(stderr,"DMA-BUF %#lx was blocked: errno=%d\n",dma[i],errno);return 1;
        }
    }
    if(getpid()<=0)return 1;
    puts("PASS: Binder transaction/control ioctls blocked; DMA-BUF sync ioctls and ordinary syscalls allowed");
    return 0;
}
