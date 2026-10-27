#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

int main(void) {
    int original=personality(0xffffffffUL);
    assert(original>=0);
    const unsigned personas[]={PER_LINUX,PER_LINUX32};
    for(unsigned i=0;i<sizeof(personas)/sizeof(personas[0]);i++) {
        assert(personality(personas[i])>=0);
        struct utsname uts;
        assert(uname(&uts)==0);
        printf("persona=%u machine=%s\n",personas[i],uts.machine);
        errno=0;
        assert(syscall(SYS_uname,(void *)1)==-1 && errno==EFAULT);
    }
    assert(personality((unsigned)original)>=0);
    return 0;
}
