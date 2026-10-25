#!/usr/bin/env python3
"""Exercise native filesystem calls through a package-to-EROFS prefix swap."""
import pathlib
import shutil
import subprocess

base=pathlib.Path(__file__).resolve().parents[1]
root=base/'build/erofs-host'
shutil.rmtree(root,ignore_errors=True)
root.mkdir(parents=True)
src=root/'fixture.c'
src.write_text(r'''#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
int main(int argc,char **argv){
 if(argc!=2)return 11;
 struct stat st,fdst,sample_st;char b[32]={0},target[64]={0};
 if(stat("/usr/sample",&st)||st.st_size!=12||(st.st_mode&0777)!=0644)return 1;
 sample_st=st;
 if(lstat("/usr/link",&st)||!S_ISLNK(st.st_mode))return 2;
 if(readlink("/usr/link",target,sizeof(target))!=6||memcmp(target,"sample",6))return 3;
 DIR *d=opendir("/usr");if(!d)return 4;int found=0;struct dirent *de;
 while((de=readdir(d)))found|=!strcmp(de->d_name,"sample");
 closedir(d);if(!found)return 5;
 int fd=open("/usr/sample",O_RDONLY|O_CLOEXEC);if(fd<0)return 6;
 if(read(fd,b,12)!=12||memcmp(b,"hello erofs\n",12)||fstat(fd,&fdst)||fdst.st_size!=sample_st.st_size)return 7;
 void *map=mmap(NULL,12,PROT_READ,MAP_PRIVATE,fd,0);if(map==MAP_FAILED||memcmp(map,"hello erofs\n",12))return 8;
 munmap(map,12);close(fd);if(stat("/usr/missing",&st)==0||errno!=ENOENT)return 9;
 /* Compare negative lookup latency in the same process and cache state. */
 const int iterations=20000;struct timespec a,z;volatile int failures=0;
 clock_gettime(CLOCK_MONOTONIC,&a);for(int i=0;i<iterations;i++)failures+=stat("/usr/missing",&st)==0;clock_gettime(CLOCK_MONOTONIC,&z);
 long mapped=(z.tv_sec-a.tv_sec)*1000000000L+z.tv_nsec-a.tv_nsec;
 clock_gettime(CLOCK_MONOTONIC,&a);for(int i=0;i<iterations;i++)failures+=stat(argv[1],&st)==0;clock_gettime(CLOCK_MONOTONIC,&z);
 long direct=(z.tv_sec-a.tv_sec)*1000000000L+z.tv_nsec-a.tv_nsec;
 printf("PASS prefix swap native open/stat/readlink/getdents/mmap; negative lookups mapped=%ldns direct=%ldns ratio=%.3f\n",mapped,direct,direct?(double)mapped/direct:0.0);
 return failures?10:0;
}''')
probe=root/'probe'
subprocess.run(['cc','-static','-O2','-Wall','-Wextra','-Werror',str(src),'-o',str(probe)],check=True)
mount=root/'mnt'/'erofs';mount.mkdir(parents=True)
(mount/'sample').write_bytes(b'hello erofs\n');(mount/'sample').chmod(0o644)
(mount/'link').symlink_to('sample')
cmd=[str(base/'build/linux-layer-v3'),'--android-filter','--erofs-mount','/usr',str(mount),str(probe),str(mount/'missing')]
result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
if result.returncode:
 raise RuntimeError(result.stdout+'\n'+result.stderr)
print(result.stdout.strip())
