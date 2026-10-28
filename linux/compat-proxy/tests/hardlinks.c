#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <linux/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#x,errno);return 1;}}while(0)
int main(int argc,char **argv){
 CHECK(argc>=2);CHECK(chdir(argv[1])==0);
 if(argc==4 && !strcmp(argv[2],"--bind")){
  int source=open("source",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(source>=0 && write(source,"logical",7)==7);close(source);
  CHECK(link("source","alias")==0);source=open("alias",O_RDONLY);CHECK(source>=0);
  struct stat st;CHECK(fstat(source,&st)==0 && st.st_nlink==2);
  char proc[80],number[32],destination[4096];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",source);snprintf(number,sizeof(number),"%d",source);
  snprintf(destination,sizeof(destination),"%s/bound",argv[1]);
  int bound=open(destination,O_CREAT|O_RDWR,0600);CHECK(bound>=0);close(bound);
  for(int mode=0;mode<2;mode++){
   pid_t child=fork();CHECK(child>=0);
   if(!child){execl(argv[3],argv[3],"--ro-bind","/","/",mode?"--ro-bind-fd":"--ro-bind",mode?number:proc,destination,
     "/bin/sh","-ec","test \"$(cat \"$1\")\" = logical; ! grep -q /[.]v3-inodes/ /proc/self/mountinfo","sh",destination,NULL);_exit(3);}
   int status;CHECK(waitpid(child,&status,0)==child && status==0);
  }
  close(source);puts("PASS logical hardlink proc-fd and bind-fd sources through stock bwrap");return 0;
 }
 if(argc==4 && !strcmp(argv[2],"--link")){CHECK(link("c",argv[3])==0);return 0;}
 if(argc==3 && !strcmp(argv[2],"--recover")){struct stat st;CHECK(stat("restored",&st)==0 && S_ISREG(st.st_mode));CHECK(stat("survivor",&st)==0 && st.st_nlink==1);int fd=open("restored",O_RDONLY);char buf[32]={0};CHECK(fd>=0 && read(fd,buf,31)==22 && !strcmp(buf,"interrupted conversion"));close(fd);puts("PASS crash conversion restoration and orphan collection");return 0;}
 if(argc==3 && !strcmp(argv[2],"--prepare")){int t=open("c",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(t>=0);close(t);return 0;}
 if(argc==3 && !strcmp(argv[2],"--check")){struct stat st;CHECK(stat("c",&st)==0 && st.st_nlink==9);puts("PASS independent loader processes share persistent link counts");return 0;}
 if(argc==3 && !strcmp(argv[2],"--markers")){
  /* Markers are prepared by the owner outside the payload. */
  int t=open("outer/deep/a",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(t>=0);close(t);
  CHECK(link("outer/deep/a","outer/deep/b")==0);
  t=open("outer/nested/deep/a",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(t>=0);close(t);
  CHECK(link("outer/nested/deep/a","outer/nested/deep/b")==0);
  errno=0;CHECK(link("outer/deep/a","sibling/b")==-1 && errno==EXDEV);
  errno=0;CHECK(link("outer/deep/a","outer/nested/c")==-1 && errno==EXDEV);
  t=open("plain/deep/a",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(t>=0);close(t);
  errno=0;CHECK(link("plain/deep/a","plain/deep/b")==-1 && errno==EACCES);
  t=open("symlink/deep/a",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(t>=0);close(t);
  errno=0;CHECK(link("symlink/deep/a","symlink/deep/b")==-1 && errno==EACCES);
  t=open("foreign/deep/a",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(t>=0);close(t);
  errno=0;CHECK(link("foreign/deep/a","foreign/deep/b")==-1 && errno==EACCES);
  const char *m="outer/.v3-store-root";
  t=open(m,O_RDONLY);CHECK(t>=0);close(t);
  DIR *d=opendir("outer");CHECK(d!=NULL);int seen=0;struct dirent *e;while((e=readdir(d)))if(!strcmp(e->d_name,".v3-store-root"))seen=1;closedir(d);CHECK(seen);
  errno=0;CHECK(open(m,O_WRONLY)==-1 && errno==EPERM);
  errno=0;CHECK(truncate(m,0)==-1 && errno==EPERM);
  errno=0;CHECK(unlink(m)==-1 && errno==EPERM);
  errno=0;CHECK(rename(m,"outer/moved")==-1 && errno==EPERM);
  errno=0;CHECK(rename("plain/deep/a",m)==-1 && errno==EPERM);
  errno=0;CHECK(link(m,"outer/alias")==-1 && errno==EPERM);
  puts("PASS marker boundaries, visibility and mutation refusal");return 0;
 }
 if(argc==4 && !strcmp(argv[2],"--escape")){errno=0;CHECK(link("a",argv[3])==-1 && errno==EXDEV);return 0;}
 if(argc==4 && !strcmp(argv[2],"--replace")){
  CHECK(rename(argv[3],"b")==0);struct stat a,b;CHECK(stat("a",&a)==0 && a.st_nlink==1 && stat("b",&b)==0 && b.st_size==3);return 0;
 }
 if(argc==3 && !strcmp(argv[2],"--placeholders")){
  int t=open("a",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(t>=0 && write(t,"content",7)==7);close(t);CHECK(link("a","b")==0);
  CHECK(chmod("a",01700)==0);CHECK(chmod("stray",0600)==0);CHECK(chmod("ordinary",01600)==0);
  struct stat st;CHECK(stat("stray",&st)==0 && (st.st_mode&S_ISVTX) && st.st_size==5);
  CHECK(stat("ordinary",&st)==0 && !(st.st_mode&S_ISVTX));
  CHECK(stat("a",&st)==0 && !(st.st_mode&S_ISVTX) && st.st_size==7 && st.st_nlink==2);
  t=open("b",O_RDWR);CHECK(t>=0);CHECK(fchmod(t,01640)==0);CHECK(fstat(t,&st)==0 && !(st.st_mode&S_ISVTX));
  errno=0;long cm=syscall(452,t,"",01640,AT_EMPTY_PATH);CHECK(cm==0 || (cm==-1 && errno==ENOSYS));CHECK(fstat(t,&st)==0 && !(st.st_mode&S_ISVTX));close(t);
  puts("PASS chmod preserves physical marker, ignores sticky changes and leaves stray sticky ordinary");return 0;
 }
 CHECK(argc==2);int fd=open("a",O_CREAT|O_RDWR|O_EXCL,0640);CHECK(fd>=0);CHECK(write(fd,"first",5)==5);CHECK(link("a","b")==0);
 struct stat a,b;CHECK(lstat("a",&a)==0 && stat("b",&b)==0 && S_ISREG(a.st_mode) && a.st_ino==b.st_ino && a.st_nlink==2);CHECK(fstat(fd,&b)==0 && b.st_nlink==2);
 struct statx sx;CHECK(statx(AT_FDCWD,"b",AT_SYMLINK_NOFOLLOW,STATX_BASIC_STATS,&sx)==0 && S_ISREG(sx.stx_mode) && sx.stx_nlink==2);CHECK(statx(fd,"",AT_EMPTY_PATH,STATX_BASIC_STATS,&sx)==0 && sx.stx_nlink==2);
 /* libglnx fallback: inspect an O_PATH no-follow fd, then reopen it. */
 int dir=open(".",O_PATH|O_DIRECTORY);CHECK(dir>=0);
 char absolute[4096];CHECK(getcwd(absolute,sizeof(absolute))!=NULL);CHECK(strlen(absolute)+3<sizeof(absolute));strcat(absolute,"/b");
 for(int i=0;i<4;i++){
  const char *name=i&1?absolute:"b";int base=i&1?AT_FDCWD:dir;
  int probe=openat(base,name,(i&2?O_RDONLY:O_PATH)|O_NOFOLLOW|O_CLOEXEC);CHECK(probe>=0);
  CHECK(statx(probe,"",AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,STATX_BASIC_STATS,&sx)==0 && S_ISREG(sx.stx_mode) && sx.stx_nlink==2);
  CHECK(fstatat(probe,"",&b,AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW)==0 && S_ISREG(b.st_mode) && b.st_nlink==2);
  CHECK(fstat(probe,&b)==0 && S_ISREG(b.st_mode) && b.st_nlink==2);
  char proc[80],content[8];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",probe);
  char logical[4096],expected[4096];ssize_t length=readlink(proc,logical,sizeof(logical)-1);CHECK(length>0);logical[length]=0;
  snprintf(expected,sizeof(expected),"%s/b",argv[1]);CHECK(!strcmp(logical,expected));
  char short_name[3];CHECK(readlink(proc,short_name,sizeof(short_name))==3 && !memcmp(short_name,expected,3));
  char *canonical=realpath(proc,NULL);CHECK(canonical && !strcmp(canonical,expected));free(canonical);
  int reopened=open(proc,O_RDONLY|O_CLOEXEC);CHECK(reopened>=0);CHECK(read(reopened,content,5)==5 && !memcmp(content,"first",5));close(reopened);
  errno=0;CHECK(readlinkat(probe,"",content,sizeof(content))==-1 && errno==ENOENT);close(probe);
 }
 char target[20];errno=0;CHECK(readlinkat(dir,"b",target,sizeof(target))==-1 && errno==EINVAL);
 CHECK(symlink("b","real-symlink")==0);CHECK(readlinkat(dir,"real-symlink",target,sizeof(target))==1 && target[0]=='b');
 int real=openat(dir,"real-symlink",O_PATH|O_NOFOLLOW);CHECK(real>=0);
 CHECK(statx(real,"",AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,STATX_BASIC_STATS,&sx)==0 && S_ISLNK(sx.stx_mode));
 CHECK(readlinkat(real,"",target,sizeof(target))==1 && target[0]=='b');close(real);
 errno=0;CHECK(openat(dir,"real-symlink",O_RDONLY|O_NOFOLLOW)==-1 && errno==ELOOP);
 CHECK(unlink("real-symlink")==0);
 CHECK(fchownat(dir,"b",getuid(),getgid(),AT_SYMLINK_NOFOLLOW)==0);
 CHECK(syscall(SYS_faccessat2,dir,"b",R_OK,AT_SYMLINK_NOFOLLOW)==0);
 /* fchmodat2 is newer than some libc headers. */
 errno=0;long cm=syscall(452,dir,"b",0640,AT_SYMLINK_NOFOLLOW);CHECK(cm==0 || (cm==-1 && errno==ENOSYS));
 CHECK(lsetxattr("b","user.nofollow","ok",2,0)==0);
 char value[256];CHECK(lgetxattr("a","user.nofollow",value,sizeof(value))==2 && !memcmp(value,"ok",2));
 CHECK(llistxattr("b",value,sizeof(value))>0);
 struct {uint64_t value;uint32_t size,flags;} xa={(uintptr_t)value,sizeof(value),0};
 errno=0;long xr=syscall(464,dir,"b",AT_SYMLINK_NOFOLLOW,"user.nofollow",&xa,sizeof(xa));
 CHECK(xr==2 || (xr==-1 && errno==ENOSYS));
 if(xr==2){
  CHECK(syscall(465,dir,"b",AT_SYMLINK_NOFOLLOW,value,sizeof(value))>0);
  xa.value=(uintptr_t)"at";xa.size=2;
  CHECK(syscall(463,dir,"a",AT_SYMLINK_NOFOLLOW,"user.nofollow-at",&xa,sizeof(xa))==0);
  CHECK(lgetxattr("b","user.nofollow-at",value,sizeof(value))==2 && !memcmp(value,"at",2));
  CHECK(syscall(466,dir,"b",AT_SYMLINK_NOFOLLOW,"user.nofollow-at")==0);
 }
CHECK(lremovexattr("a","user.nofollow")==0);
 struct {struct file_handle h;unsigned char data[128];} handle={.h.handle_bytes=128};int mount_id;
 errno=0;long hr=name_to_handle_at(dir,"b",&handle.h,&mount_id,0);int he=errno;
 /* Compare no-follow and follow calls, including filesystems without handles. */
 struct {struct file_handle h;unsigned char data[128];} follow_handle={.h.handle_bytes=128};int follow_mount;
 errno=0;long fr=name_to_handle_at(dir,"b",&follow_handle.h,&follow_mount,AT_SYMLINK_FOLLOW);
 CHECK(hr==fr && (hr==0?(mount_id==follow_mount && handle.h.handle_bytes==follow_handle.h.handle_bytes && !memcmp(handle.data,follow_handle.data,handle.h.handle_bytes)):he==errno));
 int notify=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);CHECK(notify>=0);
 int watch=inotify_add_watch(notify,"b",IN_MODIFY|IN_DONT_FOLLOW);CHECK(watch>=0);
 CHECK(pwrite(fd,"first",5,0)==5);char events[256];CHECK(read(notify,events,sizeof(events))>0);close(notify);
 close(dir);
 char buf[20];errno=0;CHECK(readlink("a",buf,sizeof(buf))==-1 && errno==EINVAL);int second=open("b",O_RDWR|O_NOFOLLOW);CHECK(second>=0);CHECK(pwrite(second,"other",5,0)==5 && pread(fd,buf,5,0)==5 && !memcmp(buf,"other",5));
 CHECK(chmod("b",0600)==0 && fstat(fd,&b)==0 && (b.st_mode&0777)==0600);struct timespec times[2]={{100,123},{200,456}};CHECK(utimensat(AT_FDCWD,"b",times,AT_SYMLINK_NOFOLLOW)==0 && fstat(fd,&b)==0 && b.st_mtim.tv_nsec==456);
 DIR *d=opendir(".");CHECK(d!=NULL);int seen=0;struct dirent *e;while((e=readdir(d))){CHECK(strcmp(e->d_name,".v3-inodes"));if(!strcmp(e->d_name,"a") || !strcmp(e->d_name,"b")){CHECK(e->d_type==DT_REG);seen++;}}closedir(d);CHECK(seen==2);errno=0;CHECK(open(".v3-inodes",O_RDONLY|O_DIRECTORY)==-1 && errno==ENOENT);
 struct stat shm; if(!stat("/dev/shm",&shm) && shm.st_dev!=a.st_dev){char target[128];snprintf(target,sizeof(target),"/dev/shm/v3-hardlinks-%ld",(long)getpid());errno=0;CHECK(link("a",target)==-1 && errno==EXDEV);}
 CHECK(rename("a","b")==0 && lstat("a",&b)==0 && b.st_nlink==2);CHECK(rename("b","c")==0 && lstat("c",&b)==0 && b.st_nlink==2);CHECK(unlink("a")==0 && fstat(fd,&b)==0 && b.st_nlink==1);
 char moved[4096];CHECK(strlen(argv[1])+7<sizeof(moved));snprintf(moved,sizeof(moved),"%s.moved",argv[1]);CHECK(rename(argv[1],moved)==0 && stat("c",&a)==0 && a.st_nlink==1);CHECK(rename(moved,argv[1])==0 && stat("c",&a)==0 && a.st_nlink==1);
 for(int i=0;i<8;i++){pid_t p=fork();CHECK(p>=0);if(!p){char name[32];snprintf(name,sizeof(name),"child-%d",i);execl(argv[0],argv[0],argv[1],"--link",name,NULL);_exit(3);}}
 for(int i=0;i<8;i++){int status;CHECK(wait(&status)>0 && status==0);}CHECK(stat("c",&a)==0 && a.st_nlink==9);
 for(int i=0;i<8;i++){char name[32];snprintf(name,sizeof(name),"child-%d",i);CHECK(unlink(name)==0);}CHECK(unlink("c")==0 && fstat(fd,&b)==0 && b.st_nlink==0);close(fd);close(second);
 puts("PASS inode store: shared writes, lstat/statx/fstat, NOFOLLOW, metadata, hidden listings, rename/unlink, fork concurrency, last-unlink");return 0;
}
