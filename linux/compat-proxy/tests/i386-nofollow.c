/* Exercise the ELF32 syscall layouts without multilib development headers. */
#include <linux/stat.h>
extern long syscall(long,...);
extern int chdir(const char *),close(int),unlink(const char *),rmdir(const char *);
extern long write(int,const void *,unsigned),read(int,void *,unsigned);
extern int *__errno_location(void);
int main(int argc,char **argv){
 if(argc!=2 || chdir(argv[1]))return 1;
 int fd=syscall(295,-100,"a",0xc2,0600);if(fd<0 || write(fd,"ok",2)!=2)return 2;close(fd);
 if(syscall(303,-100,"a",-100,"b",0))return 3;
 fd=syscall(295,-100,"b",0x200000|0x20000|0x80000,0);if(fd<0)return 4;
 struct statx st;if(syscall(383,fd,"",0x1000|0x100,0x7ff,&st) || (st.stx_mode&0170000)!=0100000 || st.stx_nlink!=2)return 5;
 char buf[8];if(syscall(305,fd,"",buf,sizeof(buf))!=-1 || *__errno_location()!=2)return 6;
 close(fd);if(syscall(304,"b",-100,"real"))return 7;
 fd=syscall(295,-100,"real",0x200000|0x20000,0);if(fd<0)return 8;
 if(syscall(383,fd,"",0x1000|0x100,0x7ff,&st) || (st.stx_mode&0170000)!=0120000 || syscall(305,fd,"",buf,sizeof(buf))!=1 || buf[0]!='b')return 9;
 close(fd);unlink("real");unlink("a");unlink("b");chdir("/");
 static const char ok[]="PASS i386 stored O_PATH NOFOLLOW statx/readlinkat and real symlink\n";write(1,ok,sizeof(ok)-1);return 0;
}
