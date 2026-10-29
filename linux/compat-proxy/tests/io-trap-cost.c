#define _GNU_SOURCE
/* Static x86_64 payload: run unchanged natively and inside an installed stub.
 * No perf permission, timestamp syscalls or per-iteration logging needed.
 * With no arguments, writes results to $HOME/io-trap-cost.txt as well as stdout.
 * Usage: trap-cost [LOOPS [OUTPUT [DIRECTORY]]]
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock_gettime"); exit(2); }
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void report(FILE *out, const char *name, double us) {
    printf("%s %.6f us/op\n", name, us);
    if (out) { fprintf(out, "%s %.6f us/op\n", name, us); fflush(out); }
}
int main(int argc, char **argv) {
    long loops = argc > 1 ? strtol(argv[1], NULL, 10) : 30000;
    if (loops <= 0 || argc > 4) return 2;
    const char *home = argc > 3 ? argv[3] : getenv("HOME");
    if (!home) home = ".";
    char output[4096], path[4096], missing[4096], link[4096];
    if (snprintf(output,sizeof(output),"%s/io-trap-cost.txt",home)>=(int)sizeof(output) ||
        snprintf(path,sizeof(path),"%s/.io-trap-cost-%ld",home,(long)getpid())>=(int)sizeof(path) ||
        snprintf(missing,sizeof(missing),"%s.missing",path)>=(int)sizeof(missing) ||
        snprintf(link,sizeof(link),"%s.link",path)>=(int)sizeof(link)) return 2;
    FILE *out = fopen(argc > 2 ? argv[2] : output,"w");
    if (!out) { perror("result file"); return 2; }
    int fd = open(path,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
    if (fd < 0) { perror("fixture"); fclose(out); return 2; }
    if (ftruncate(fd,loops+4096) || symlink(path,link)) { perror("fixture setup"); close(fd); unlink(path); fclose(out); return 2; }
    const char *names[]={"getpid-native","clock-syscall-native","ioctl-emulated","flock-trap-raw",
        "fcntl-trap-raw","read-zero","write-zero","read","write","openat-close","statx",
        "access","readlink","openat-miss","statx-miss","access-miss","readlink-miss"};
    int failed=0;
    /* Multiple rounds expose first-trap setup versus steady-state costs. */
    for(int round=0;round<3 && !failed;round++) {
        fprintf(out,"round %d loops %ld\n",round,loops);
        for(unsigned op=0;op<sizeof(names)/sizeof(names[0]);op++) {
            char buf[4096]; struct statx st; struct timespec ts;
            double start=now();
            for(long i=0;i<loops;i++) {
                long r;
                switch(op) {
                case 0:r=syscall(SYS_getpid);break;
                case 1:r=syscall(SYS_clock_gettime,CLOCK_MONOTONIC,&ts);break;
                case 2:r=syscall(SYS_ioctl,fd,FICLONE,-1);break;
                case 3:r=syscall(SYS_flock,fd,LOCK_UN);break;
                case 4:r=syscall(SYS_fcntl,fd,F_GETFL);break;
                case 5:r=syscall(SYS_read,fd,buf,0);break;
                case 6:r=syscall(SYS_write,fd,buf,0);break;
                case 7:r=syscall(SYS_read,fd,buf,1);break;
                case 8:r=syscall(SYS_write,fd,"x",1);break;
                case 9:case 13:r=syscall(SYS_openat,AT_FDCWD,op==9?path:missing,O_RDONLY|O_CLOEXEC,0);if(r>=0)close((int)r);break;
                case 10:case 14:r=syscall(SYS_statx,AT_FDCWD,op==10?path:missing,0,STATX_BASIC_STATS,&st);break;
                case 11:case 15:r=syscall(SYS_faccessat2,AT_FDCWD,op==11?path:missing,F_OK,0);break;
                default:r=syscall(SYS_readlinkat,AT_FDCWD,op==12?link:missing,buf,sizeof(buf));break;
                }
                /* Native FICLONE(-1) is EBADF; proxy deliberately returns
                 * EOPNOTSUPP without entering the kernel. Both are expected. */
                int ok=op==2 ? (r==-1 && (errno==EBADF || errno==EOPNOTSUPP || errno==ENOTTY)) :
                    op>=13 ? (r==-1 && errno==ENOENT) : op==7 || op==8 ? r==1 : r>=0;
                if(!ok) { fprintf(stderr,"%s failed r=%ld errno=%d\n",names[op],r,errno);failed=1;break; }
            }
            report(out,names[op],(now()-start)*1e6/loops);
            if(op==7 || op==8) lseek(fd,0,SEEK_SET);
            if(failed)break;
        }
    }
    close(fd); unlink(link); unlink(path);
    if(!failed)fprintf(out,"complete\n");
    fclose(out);
    return failed ? 1 : 0;
}
