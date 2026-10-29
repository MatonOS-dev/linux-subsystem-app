/* Runtime-only glibc fixture: exercise the actual AT_SYSINFO entry, including
 * older glibcs which use it even without AT_SYSINFO_EHDR. */
extern int open(const char *,int,...),close(int),isatty(int),*__errno_location(void);
extern long terminal_vsyscall(long,long,long,long,long,long,long);
extern int printf(const char *,...);
unsigned long terminal_sysinfo;
int main(int argc,char **argv,char **env) {
 (void)argc;(void)argv;
 while(*env)env++;
 for(unsigned long *aux=(void *)(env+1);aux[0];aux+=2)
  if(aux[0]==32)terminal_sysinfo=aux[1];
 if(!terminal_sysinfo)return 1;
 int fd=open("/proc/self/mountinfo",0);if(fd<0)return 2;
 for(int i=0;i<100;i++) {
  long result=terminal_vsyscall(54,fd,0x5401,1,0,0,0);
  if(result!=-25){printf("FAIL sysinfo=%lx result=%ld fd=%d\n",terminal_sysinfo,result,fd);return 3;}
  *__errno_location()=0;
  if(isatty(fd) || *__errno_location()!=25)return 4;
 }
 close(fd);
 fd=open("/dev/null",0);if(fd<0)return 5;
 /* The underlying filter is really active; character devices retain policy. */
 if(terminal_vsyscall(54,fd,0x5401,1,0,0,0)!=-13)return 6;
 close(fd);
 printf("PASS i386 AT_SYSINFO terminal queries: ENOTTY without kernel ioctl\n");
 return 0;
}
