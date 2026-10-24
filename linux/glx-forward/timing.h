/* Linux syscall ABI used by GLX frame pacing. No libc TLS is required. */
struct timespec { long tv_sec, tv_nsec; };
#if defined(__i386__)
/* Linux i386 uses int 0x80 and the time32 wire layout above. The x86_64
 * syscall instruction/register ABI cannot be used in a compat process. */
static long sys_clock_gettime(long id,struct timespec*t){long r;__asm__ volatile("int $0x80":"=a"(r):"0"(265),"b"(id),"c"(t):"memory");return r;}
static long sys_nanosleep(const struct timespec*t){long r;__asm__ volatile("int $0x80":"=a"(r):"0"(162),"b"(t),"c"(0):"memory");return r;}
#else
static long sys_clock_gettime(long id,struct timespec*t){long r;__asm__ volatile("syscall":"=a"(r):"0"(228),"D"(id),"S"(t):"rcx","r11","memory");return r;}
static long sys_nanosleep(const struct timespec*t){long r;__asm__ volatile("syscall":"=a"(r):"0"(35),"D"(t):"rcx","r11","memory");return r;}
#endif
