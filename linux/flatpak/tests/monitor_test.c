#define main wrapper_main
#include "../flatpak-env-wrapper.c"
#undef main
#include <assert.h>

int main(void) {
    char root[]="/tmp/maton-r10-monitor-XXXXXX";
    assert(mkdtemp(root));
    char display[128],monitor[192],file[256],text[512];
    snprintf(display,sizeof(display),"%s/wayland-1",root);
    assert(prepare_monitor(display,"10.0.2.3,2001:db8::53",monitor,sizeof(monitor))==0);
    snprintf(file,sizeof(file),"%s/resolv.conf",monitor);
    int fd=open(file,O_RDONLY);assert(fd>=0);
    ssize_t size=read(fd,text,sizeof(text)-1);assert(size>0);text[size]=0;close(fd);
    assert(!strcmp(text,"nameserver 10.0.2.3\nnameserver 2001:db8::53\n"));
    assert(prepare_monitor(display,"fe80::53%wlan0,fe80::54%2",monitor,sizeof(monitor))==0);
    fd=open(file,O_RDONLY);assert(fd>=0);
    size=read(fd,text,sizeof(text)-1);assert(size>0);text[size]=0;close(fd);
    assert(!strcmp(text,"nameserver fe80::53%wlan0\nnameserver fe80::54%2\n"));
    assert(prepare_monitor(display,"fe80::53%wlan0\noptions bad",monitor,sizeof(monitor))!=0);
    assert(prepare_monitor(display,"10.0.2.3%wlan0",monitor,sizeof(monitor))!=0);
    assert(prepare_monitor(display,"fe80::53%",monitor,sizeof(monitor))!=0);
    assert(prepare_monitor(display,"10.0.2.3\noptions bad",monitor,sizeof(monitor))!=0);
    assert(prepare_monitor(display,"not-an-address",monitor,sizeof(monitor))!=0);
    assert(chmod(monitor,0777)==0);
    assert(prepare_monitor(display,"10.0.2.3",monitor,sizeof(monitor))!=0);
    assert(chmod(monitor,0700)==0);
    assert(unlink(file)==0 && symlink("/dev/null",file)==0);
    assert(prepare_monitor(display,"10.0.2.3",monitor,sizeof(monitor))!=0);
    const char* files[]={"resolv.conf","hosts","host.conf","gai.conf"};
    for(unsigned i=0;i<4;i++){snprintf(file,sizeof(file),"%s/%s",monitor,files[i]);unlink(file);}
    rmdir(monitor);rmdir(root);
    puts("DNS monitor validation passed");
    return 0;
}
