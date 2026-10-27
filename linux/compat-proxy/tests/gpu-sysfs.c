#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <sys/mount.h>
#include "../vfs.h"
#define CHECK(x) do{if(!(x)){perror(#x);exit(1);}}while(0)
static void contents(int dir,const char *name,const char *want){char b[256];int fd=openat(dir,name,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,b,sizeof(b)-1);CHECK(n>=0);b[n]=0;CHECK(!strcmp(b,want));close(fd);}
int main(int argc,char **argv){
    if(argc==3 && !strcmp(argv[1],"--publish")){CHECK(!vfs_gpu_publish(argv[2],"0x8086","0x5912","i915","renderD129",129,"card2",2));return 0;}

    /* libpci starts by enumerating the PCI bus, rather than /sys/class/drm.
     * A missing devices directory makes its default error callback exit(1). */
    DIR *pci=opendir("/sys/bus/pci/devices");CHECK(pci);
    struct dirent *entry;unsigned devices=0;
    while((entry=readdir(pci)))if(entry->d_name[0]!='.'){
        CHECK(!strcmp(entry->d_name,"0000:00:00.0"));devices++;
        int dev=openat(dirfd(pci),entry->d_name,O_RDONLY|O_DIRECTORY);CHECK(dev>=0);
        contents(dev,"vendor","0x8086\n");contents(dev,"device","0x5912\n");
        contents(dev,"class","0x030000\n");close(dev);
    }
    CHECK(devices==1);closedir(pci);
    char path[256],link[256];struct stat a,b;
    CHECK(!stat("/sys/bus/pci/devices/0000:00:00.0",&a));
    CHECK(!stat("/sys/devices/pci0000:00/0000:00:00.0",&b));CHECK(a.st_ino==b.st_ino);
    const char *nodes[]={"226:129","226:2"};
    const char *classes[]={"renderD129","card2"};
    for(unsigned i=0;i<2;i++){
        snprintf(path,sizeof(path),"/sys/class/drm/%s",classes[i]);
        CHECK(!lstat(path,&a) && S_ISLNK(a.st_mode));
        CHECK(!stat(path,&a));
        snprintf(path,sizeof(path),"/sys/dev/char/%s",nodes[i]);
        CHECK(!stat(path,&b) && a.st_ino==b.st_ino);
        snprintf(path,sizeof(path),"/sys/dev/char/%s",nodes[i]);
        CHECK(!lstat(path,&a) && S_ISLNK(a.st_mode));ssize_t alias=readlink(path,link,sizeof(link)-1);CHECK(alias>0);link[alias]=0;CHECK(strstr(link,"../../devices/pci0000:00/0000:00:00.0/drm/")==link);
        snprintf(path,sizeof(path),"/sys/dev/char/%s/device",nodes[i]);char *real=realpath(path,NULL);CHECK(real);CHECK(!strcmp(real,"/sys/devices/pci0000:00/0000:00:00.0"));free(real);
        int fd=open(path,O_RDONLY|O_DIRECTORY);CHECK(fd>=0);
        contents(fd,"vendor","0x8086\n");contents(fd,"device","0x5912\n");contents(fd,"subsystem_vendor","0x0000\n");contents(fd,"subsystem_device","0x0000\n");contents(fd,"revision","0x00\n");contents(fd,"class","0x030000\n");
        contents(fd,"uevent","DRIVER=i915\nPCI_ID=8086:5912\nPCI_SLOT_NAME=0000:00:00.0\n");
        ssize_t n=readlinkat(fd,"subsystem",link,sizeof(link)-1);CHECK(n>0);link[n]=0;CHECK(!strcmp(link,"../../../bus/pci"));
        CHECK(!fstatat(fd,"subsystem",&a,0));CHECK(!stat("/sys/bus/pci",&b));CHECK(a.st_ino==b.st_ino);
        int drm=openat(fd,"drm",O_RDONLY|O_DIRECTORY);CHECK(drm>=0);DIR *d=fdopendir(drm);CHECK(d);unsigned found=0;struct dirent *ent;while((ent=readdir(d)))if(!strcmp(ent->d_name,"card2")||!strcmp(ent->d_name,"renderD129"))found++;CHECK(found==2);closedir(d);
        struct statx sx;CHECK(!statx(fd,"vendor",0,STATX_TYPE|STATX_SIZE,&sx));CHECK(S_ISREG(sx.stx_mode) && sx.stx_size==7);
        CHECK(openat(fd,"vendor",O_WRONLY)==-1 && errno==EROFS);close(fd);
    }
    int fd=open("/sys/dev/char/226:129",O_RDONLY|O_DIRECTORY);CHECK(fd>=0);contents(fd,"uevent","MAJOR=226\nMINOR=129\nDEVNAME=dri/renderD129\n");close(fd);
    struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);
    for(unsigned i=0;i<20000;i++){int bus=open("/sys/bus",O_RDONLY|O_DIRECTORY),cls=open("/sys/class",O_RDONLY|O_DIRECTORY);CHECK(bus>=0&&cls>=0);close(bus);close(cls);}
    clock_gettime(CLOCK_MONOTONIC,&end);printf("PASS GPU libdrm read sequence; 40000 sysfs opens %.3f s\n",end.tv_sec-start.tv_sec+(end.tv_nsec-start.tv_nsec)/1e9);
    return 0;
}
