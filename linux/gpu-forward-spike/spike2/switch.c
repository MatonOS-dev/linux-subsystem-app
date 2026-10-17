#define _GNU_SOURCE
#include "switch.h"
#include <sys/auxv.h>
#include <unistd.h>
#include <pthread.h>
int gs_fsgsbase;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int forced;
static void initialize(void) {
    gs_fsgsbase = !forced && (getauxval(AT_HWCAP2) & 2);
    if (!gs_fsgsbase) {
        static const char message[] = "maton GPU: FSGSBASE unavailable; using arch_prctl\n";
        (void)gs_raw(1, 2, (long)message, sizeof(message)-1);
    }
}
void gs_init(int force_fallback) {
    /* The force flag is a process-start test option, not a runtime toggle. */
    if (force_fallback) forced = 1;
    pthread_once(&once, initialize);
}
