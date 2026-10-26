#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    /* Verify Linux's query, set return, and subsequent previous-value
     * behavior. Under Android's syscall filter the proxy may emulate the set;
     * on desktop Linux the kernel may perform it. */
    int before=personality(0xffffffffUL);
    assert(before>=0);
    errno=0;
    long old=syscall(SYS_personality,PER_LINUX32);
    assert(old==before);
    assert(personality(0xffffffffUL)==PER_LINUX32);
    old=syscall(SYS_personality,PER_LINUX);
    assert(old==PER_LINUX32);
    assert(personality(0xffffffffUL)==PER_LINUX);
    /* Container/Android syscall filters commonly refuse ADDR_NO_RANDOMIZE;
     * the proxy must still remember the requested full persona. */
    old=syscall(SYS_personality,ADDR_NO_RANDOMIZE);
    assert(old==PER_LINUX);
    assert(personality(0xffffffffUL)==ADDR_NO_RANDOMIZE);
    old=syscall(SYS_personality,PER_LINUX);
    assert(old==ADDR_NO_RANDOMIZE);
    assert(personality(0xffffffffUL)==PER_LINUX);
    puts("PASS personality query/set/previous semantics follow Linux for arbitrary persona requests");
    return 0;
}
