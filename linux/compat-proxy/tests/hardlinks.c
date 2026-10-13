#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <linux/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#x,errno);return 1;}}while(0)
int main(int argc,char **argv){
 CHECK(argc>=2);CHECK(chdir(argv[1])==0);
 if(argc==4 && !strcmp(argv[2],"--link")){CHECK(link("c",argv[3])==0);return 0;}
 if(argc==3 && !strcmp(argv[2],"--recover")){struct stat st;CHECK(stat("restored",&st)==0 && S_ISREG(st.st_mode));CHECK(stat("survivor",&st)==0 && st.st_nlink==1);puts("PASS crash conversion restoration and orphan collection");return 0;}
 if(argc==3 && !strcmp(argv[2],"--prepare")){int t=open("c",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(t>=0);close(t);return 0;}
 if(argc==3 && !strcmp(argv[2],"--check")){struct stat st;CHECK(stat("c",&st)==0 && st.st_nlink==9);puts("PASS independent loader processes share persistent link counts");return 0;}
 CHECK(argc==2);int fd=open("a",O_CREAT|O_RDWR|O_EXCL,0640);CHECK(fd>=0);CHECK(write(fd,"first",5)==5);CHECK(link("a","b")==0);
 struct stat a,b;CHECK(lstat("a",&a)==0 && stat("b",&b)==0 && S_ISREG(a.st_mode) && a.st_ino==b.st_ino && a.st_nlink==2);CHECK(fstat(fd,&b)==0 && b.st_nlink==2);
 struct statx sx;CHECK(statx(AT_FDCWD,"b",AT_SYMLINK_NOFOLLOW,STATX_BASIC_STATS,&sx)==0 && S_ISREG(sx.stx_mode) && sx.stx_nlink==2);CHECK(statx(fd,"",AT_EMPTY_PATH,STATX_BASIC_STATS,&sx)==0 && sx.stx_nlink==2);
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
