#!/usr/bin/env python3
"""Pressure-vessel shape: many bind aliases and directory-relative scans."""
import pathlib, subprocess, tempfile
base = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mount-scan-', dir=base/'build') as scratch:
    root = pathlib.Path(scratch)
    source = root/'probe.c'
    source.write_text(r'''#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
int main(int argc,char **argv){
 if(argc!=2)return 1;
 char path[256];
 for(int i=0;i<512;i++){
  snprintf(path,sizeof(path),"/scan/extensions/mount-%d",i);
  if(mount(argv[1],path,0,MS_BIND,0))return 2;
 }
 int dir=open("/scan/extensions/mount-0",O_RDONLY|O_DIRECTORY);if(dir<0)return 3;
 struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
 for(int i=0;i<10000;i++){
  int fd=openat(dir,"probe.c",O_RDONLY|O_NOFOLLOW);if(fd<0)return 4;
  struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode))return 5;
  close(fd);
 }
 clock_gettime(CLOCK_MONOTONIC,&b);
 printf("PASS 512-mount relative scan: %ld ns/iteration\n",((b.tv_sec-a.tv_sec)*1000000000L+b.tv_nsec-a.tv_nsec)/10000);
 return 0;
}''')
    probe=root/'scan'
    subprocess.run(['cc','-static','-O2','-Wall','-Wextra','-Werror',str(source),'-o',str(probe)],check=True)
    result=subprocess.run([str(base/'build/linux-layer-v3'),'--android-filter',str(probe),str(root)],capture_output=True,text=True,timeout=60)
    if result.returncode:raise RuntimeError(result.stderr)
    print(result.stdout.strip())
