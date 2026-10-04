#define execv capture_execv
#define main shim_main
#include "../matonos-bwrap.c"
#undef main
#undef execv
#include <assert.h>
#include <sys/mman.h>

static void test_option_values(void) {
    assert(option_values("--overlay")==3);
    assert(option_values("--overlay-src")==1);
    assert(option_values("--tmp-overlay")==1);
    assert(option_values("--ro-overlay")==1);
    assert(option_values("--level-prefix")==0);
    assert(option_values("--not-a-security-boundary")==0);
    assert(option_values("--bind")==2);
    assert(option_values("--ro-bind-try")==2);
    assert(option_values("--dev-bind")==2);
    assert(option_values("--bind-fd")==2);
    assert(option_values("--file")==2);
    assert(option_values("--symlink")==2);
    assert(option_values("--chmod")==2);
    assert(option_values("--setenv")==2);
    assert(option_values("--args")==1);
    assert(option_values("--argv0")==1);
    assert(option_values("--tmpfs")==1);
    assert(option_values("--dir")==1);
    assert(option_values("--unsetenv")==1);
    assert(option_values("--uid")==1);
    assert(option_values("--gid")==1);
    assert(option_values("--cap-add")==1);
    assert(option_values("--cap-drop")==1);
    assert(option_values("--perms")==1);
    assert(option_values("--size")==1);
    assert(option_values("--userns2")==1);
    assert(option_values("--json-status-fd")==1);
    assert(option_values("--seccomp")==1);
    assert(option_values("--unshare-all")==0);
    assert(option_values("--share-net")==0);
    assert(option_values("--clearenv")==0);
    assert(option_values("--new-session")==0);
    assert(option_values("--die-with-parent")==0);
    assert(option_values("--as-pid-1")==0);
    assert(option_values("--unknown-option")==0);
}

static void test_scan_argv(void) {
    char* flatpak_run[]={"/system_ext/bin/matonos-bwrap","--args","3","--","app","--flag",NULL};
    char* direct[]={"/system_ext/bin/matonos-bwrap","--unshare-all","--bind","/a","/b","--setenv","X","Y","--tmpfs","/t","app","arg",NULL};
    char* proxy[]={"/system_ext/bin/matonos-bwrap","--args","5","--","xdg-dbus-proxy","--args=9",NULL};
    char* trap[]={"/system_ext/bin/matonos-bwrap","--setenv","--args","3","app",NULL};
    char* perms[]={"/system_ext/bin/matonos-bwrap","--perms","0755","--tmpfs","/t","app",NULL};
    char* bare[]={"/system_ext/bin/matonos-bwrap","app",NULL};
    int args_end,dashdash,command;

    command=scan_argv(6,flatpak_run,&args_end,&dashdash);
    assert(args_end==3 && dashdash==3 && command==4);

    command=scan_argv(11,direct,&args_end,&dashdash);
    assert(args_end==-1 && dashdash==-1 && command==10);

    command=scan_argv(6,proxy,&args_end,&dashdash);
    /* the trailing --args=9 belongs to the proxy command */
    assert(args_end==3 && dashdash==3 && command==4);

    command=scan_argv(5,trap,&args_end,&dashdash);
    /* --args was consumed as the --setenv value, not an option */
    assert(args_end==-1 && dashdash==-1 && command==4);

    command=scan_argv(6,perms,&args_end,&dashdash);
    assert(args_end==-1 && dashdash==-1 && command==5);

    command=scan_argv(2,bare,&args_end,&dashdash);
    assert(args_end==-1 && dashdash==-1 && command==1);
}

/* Overlay values must not masquerade as options or the command. This also
 * checks bundled FD scanning reaches real environment entries after overlays. */
static void test_overlay_args(void) {
    char* argv[]={"shim","--overlay-src","/lower","--overlay","--args",
                  "--setenv","/merged","--level-prefix","--not-a-security-boundary",
                  "--tmp-overlay","/tmp","--ro-overlay","/ro","--","app",NULL};
    int args_end,dashdash;
    assert(scan_argv(15,argv,&args_end,&dashdash)==14);
    assert(args_end==-1 && dashdash==13);
    const char data[]="--overlay-src\0/lower\0--overlay\0--setenv\0MATON_X11_SOCKET\0/fake\0"
                      "--tmp-overlay\0/tmp\0--ro-overlay\0/ro\0--tmpfs\0/tmp/.X11-unix\0"
                      "--setenv\0MATON_X11_SOCKET\0/real\0";
    int fd=memfd_create("overlay-args",MFD_CLOEXEC);assert(fd>=0);
    assert(write(fd,data,sizeof(data)-1)==sizeof(data)-1);
    assert(lseek(fd,0,SEEK_SET)==0);
    int tmpfs=0,app=0;char* journal=NULL;
    char* found=args_fd_x11_socket(fd,&tmpfs,&journal,&app);
    assert(found && !strcmp(found,"/real") && tmpfs);
    assert(lseek(fd,0,SEEK_CUR)==0);close(fd);
}

static void test_insert_x11_args(void) {
    char* argv[]={"/system_ext/bin/matonos-bwrap","--args","3","--","app",NULL};
    char* extra[]={"--bind","/data/matonos/linux/runtime/wayland-1-x11","/tmp/.X11-unix/X0","--setenv","DISPLAY",":0"};
    char** extended=insert_args(5,argv,3,extra,6);
    int i;
    assert(extended);
    assert(!strcmp(extended[0],"/system_ext/bin/matonos-bwrap"));
    assert(!strcmp(extended[1],"--args"));
    assert(!strcmp(extended[2],"3"));
    assert(!strcmp(extended[3],"--bind"));
    assert(!strcmp(extended[4],"/data/matonos/linux/runtime/wayland-1-x11"));
    assert(!strcmp(extended[5],"/tmp/.X11-unix/X0"));
    assert(!strcmp(extended[6],"--setenv"));
    assert(!strcmp(extended[7],"DISPLAY"));
    assert(!strcmp(extended[8],":0"));
    assert(!strcmp(extended[9],"--"));
    assert(!strcmp(extended[10],"app"));
    assert(extended[11]==NULL);
    for(i=0;i<11;i++)assert(extended[i]);
    free(extended);
}

static void test_args_fd_x11_socket(void) {
    const char data[]="--unshare-all\0--tmpfs\0/tmp/.X11-unix\0--unsetenv\0DISPLAY\0"
                      "--setenv\0WAYLAND_DISPLAY\0/data/matonos/linux/runtime/wayland-1\0"
                      "--setenv\0MATON_X11_SOCKET\0/data/matonos/linux/runtime/wayland-1-x11\0";
    char* found;
    int tmpfs=0, app=0;char* journal=NULL;
    int fd=memfd_create("bwrap-args",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    assert(fd>=0);
    assert(ftruncate(fd,sizeof(data)-1)==0);
    assert(write(fd,data,sizeof(data)-1)==(ssize_t)(sizeof(data)-1));
    assert(lseek(fd,0,SEEK_SET)==0);
    found=args_fd_x11_socket(fd,&tmpfs,&journal,&app);
    assert(found && !strcmp(found,"/data/matonos/linux/runtime/wayland-1-x11"));
    assert(tmpfs);
    /* the real bwrap must still read the arguments from the start */
    assert(lseek(fd,0,SEEK_CUR)==0);
    close(fd);

    /* no MATON_X11_SOCKET triplet: nothing is found, offset restored */
    const char plain[]="--unshare-all\0--tmpfs\0/tmp/.X11-unix\0";
    fd=memfd_create("bwrap-args",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    assert(fd>=0);
    assert(ftruncate(fd,sizeof(plain)-1)==0);
    assert(write(fd,plain,sizeof(plain)-1)==(ssize_t)(sizeof(plain)-1));
    assert(lseek(fd,0,SEEK_SET)==0);
    tmpfs=0;
    assert(args_fd_x11_socket(fd,&tmpfs,&journal,&app)==NULL);
    assert(tmpfs);
    assert(lseek(fd,0,SEEK_CUR)==0);
    close(fd);

    /* helper sandbox (xdg-dbus-proxy): socket named, no X11 tmpfs */
    const char helper[]="--ro-bind\0/\0/\0--setenv\0MATON_X11_SOCKET\0/x\0";
    fd=memfd_create("bwrap-args",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    assert(fd>=0);
    assert(write(fd,helper,sizeof(helper)-1)==(ssize_t)(sizeof(helper)-1));
    assert(lseek(fd,0,SEEK_SET)==0);
    tmpfs=0;
    assert(args_fd_x11_socket(fd,&tmpfs,&journal,&app));
    assert(!tmpfs);
    close(fd);
}

static char** captured;
int capture_execv(const char* path,char* const* argv) {
    assert(!strcmp(path,BWRAP));
    unsigned n=0;while(argv[n])n++;
    captured=calloc(n+1,sizeof(char*));assert(captured);
    for(unsigned i=0;i<n;i++)captured[i]=strdup(argv[i]);
    errno=ENOENT;return -1;
}
static void test_seccomp_forwarding(void) {
    int filter=memfd_create("filter",0);assert(filter>=0);
    assert(write(filter,"BPF",3)==3);assert(lseek(filter,0,SEEK_SET)==0);
    char number[24];snprintf(number,sizeof(number),"%d",filter);
    char* args[]={"shim","--seccomp",number,"--","app",NULL};
    assert(shim_main(5,args)==127);
    assert(!strcmp(captured[1],"--seccomp"));assert(!strcmp(captured[2],number));
    assert(fcntl(filter,F_GETFD)>=0);assert(lseek(filter,0,SEEK_CUR)==0);
    for(unsigned i=0;captured[i];i++)free(captured[i]);free(captured);
    /* Flatpak normally bundles --seccomp into the inherited --args FD. */
    int fd=memfd_create("args",0);assert(fd>=0);
    const char bundled[]="--seccomp\0";
    assert(write(fd,bundled,sizeof(bundled)-1)==sizeof(bundled)-1);
    assert(write(fd,number,strlen(number)+1)==(ssize_t)strlen(number)+1);
    assert(lseek(fd,0,SEEK_SET)==0);
    snprintf(number,sizeof(number),"%d",fd);
    char* packed[]={"shim","--args",number,"--","app",NULL};
    assert(shim_main(5,packed)==127);
    assert(!strcmp(captured[1],"--args"));assert(!strcmp(captured[2],number));
    assert(lseek(fd,0,SEEK_CUR)==0);
    char contents[64];assert(read(fd,contents,sizeof(contents))>0);
    assert(!strcmp(contents,"--seccomp"));
    for(unsigned i=0;captured[i];i++)free(captured[i]);free(captured);close(fd);close(filter);
}
static void test_machine_id_binds(void) {
    const char app[]="--dir\0/var\0--setenv\0FLATPAK_ID\0org.example.AnyApp\0";
    const char helper[]="--ro-bind\0/\0/\0";
    for(unsigned test=0;test<2;test++) {
        int fd=memfd_create("bwrap-args",MFD_CLOEXEC);assert(fd>=0);
        const char* data=test?helper:app;size_t size=test?sizeof(helper)-1:sizeof(app)-1;
        assert(write(fd,data,size)==(ssize_t)size);assert(lseek(fd,0,SEEK_SET)==0);
        char number[24];snprintf(number,sizeof(number),"%d",fd);
        char* args[]={"shim","--args",number,"--","app",NULL};
        assert(shim_main(5,args)==127);
        unsigned ids=0;
        for(unsigned i=0;captured[i];i++)if(!strcmp(captured[i],"/data/matonos/linux/machine-id")) {
            assert(i>0&&!strcmp(captured[i-1],"--ro-bind"));
            assert(!strcmp(captured[i+1],ids?"/var/lib/dbus/machine-id":"/etc/machine-id"));ids++;
        }
        assert(ids==(test?0:2));assert(lseek(fd,0,SEEK_CUR)==0);
        for(unsigned i=0;captured[i];i++)free(captured[i]);free(captured);close(fd);
    }
}
int main(void) {
    test_seccomp_forwarding();
    test_machine_id_binds();
    test_option_values();
    test_overlay_args();
    test_scan_argv();
    test_insert_x11_args();
    test_args_fd_x11_socket();
    puts("bwrap shim validation passed");
    return 0;
}
