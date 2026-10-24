typedef unsigned long size_t;
typedef struct { long sec,nsec; } timespec;
extern long i386_vsyscall6(long,long,long,long,long,long,long);
extern int clock_gettime(int,timespec *);
extern int printf(const char *,...);
extern long strtol(const char *,char **,int);
#define AT_FDCWD -100
#define O_RDONLY 0
static int stat64(const char *p,unsigned char *st){return (int)i386_vsyscall6(195,(long)p,(long)st,0,0,0,0);}
static int statx(const char *p,unsigned char *st){return (int)i386_vsyscall6(383,AT_FDCWD,(long)p,0,0x7ff,(long)st,0);}
static long elapsed(timespec a,timespec b){return (b.sec-a.sec)*1000000000L+b.nsec-a.nsec;}
static void bench(const char *name,const char *path,long count){
    unsigned char st[256];timespec a,b;long r=0;
    clock_gettime(1,&a);
    for(long i=0;i<count;i++){
        if(name[0]=='o') {r=i386_vsyscall6(295,AT_FDCWD,(long)path,O_RDONLY,0,0,0);if(r>=0)r=i386_vsyscall6(6,r,0,0,0,0,0);}
        else if(name[0]=='s')r=stat64(path,st);
        else r=statx(path,st);
        if(r<0)break;
    }
    clock_gettime(1,&b);
    if(r<0){printf("ERROR %s %ld\n",name,r);return;}
    printf("%s %ld\n",name,elapsed(a,b)/count);
}
int main(int argc,char **argv){
    if(argc<2)return 2;
    char *end=0;
    long count=argc>2?strtol(argv[2],&end,10):5000;
    if(count<1)return 2;
    bench("openat+close",argv[1],count);bench("stat64",argv[1],count);bench("statx",argv[1],count);return 0;
}
