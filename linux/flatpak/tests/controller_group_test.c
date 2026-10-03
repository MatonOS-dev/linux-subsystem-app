#define _GNU_SOURCE
#include <assert.h>
#include <grp.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <linux/capability.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdarg.h>

static gid_t inherited[4];
static int inherited_count, assigned_count, fail_setgroups;
static gid_t assigned[4];
static int fake_getgroups(int size, gid_t* groups) {
    assert(size >= inherited_count);
    memcpy(groups, inherited, sizeof(gid_t) * inherited_count);
    return inherited_count;
}
static int fake_setgroups(size_t count, const gid_t* groups) {
    if (fail_setgroups) { errno = EPERM; return -1; }
    assigned_count = count;
    memcpy(assigned, groups, count * sizeof(gid_t));
    return 0;
}
static long fake_syscall(long number, ...) {
    va_list args;
    va_start(args, number);
    (void)va_arg(args, struct __user_cap_header_struct*);
    struct __user_cap_data_struct* caps = va_arg(args, struct __user_cap_data_struct*);
    va_end(args);
    if (number == SYS_capget) {
        caps[0].effective = caps[0].permitted = caps[0].inheritable = 1U << CAP_SETGID;
    } else {
        assert(number == SYS_capset);
        assert(!(caps[0].effective & (1U << CAP_SETGID)));
        assert(!(caps[0].permitted & (1U << CAP_SETGID)));
        assert(!(caps[0].inheritable & (1U << CAP_SETGID)));
    }
    return 0;
}
#define getgroups fake_getgroups
#define setgroups fake_setgroups
#define syscall fake_syscall
#include "../controller-access.h"

int main(void) {
    inherited[0] = 1000; inherited[1] = 3003; inherited_count = 2;
    setenv("MATON_GAME_CONTROLLERS", "1", 1);
    assert(configure_controller_group() == 0);
    assert(assigned_count == 3 && assigned[2] == MATON_CONTROLLER_GID);
    assert(!getenv("MATON_GAME_CONTROLLERS"));
    inherited[2] = MATON_CONTROLLER_GID; inherited_count = 3;
    assert(controller_group_present());
    setenv("MATON_GAME_CONTROLLERS", "0", 1);
    assert(configure_controller_group() == 0);
    assert(assigned_count == 2 && assigned[0] == 1000 && assigned[1] == 3003);
    assigned_count = -1;
    assert(configure_controller_group() == 0); /* nested launch keeps its grant */
    assert(assigned_count == -1);
    setenv("MATON_GAME_CONTROLLERS", "all", 1);
    assert(configure_controller_group() == -1 && errno == EINVAL);
    setenv("MATON_GAME_CONTROLLERS", "1", 1); fail_setgroups = 1;
    assert(configure_controller_group() == -1 && errno == EPERM);
    return 0;
}
