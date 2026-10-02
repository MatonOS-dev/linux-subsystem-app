#define _GNU_SOURCE
/*
 * MatonOS launcher for the NDK-built Flatpak CLI. linuxd execs the stable
 * /system_ext/bin/flatpak path; give its GPGME and bwrap subprocesses the
 * image paths they need, then preserve argv[0] and replace this process with
 * the tested CLI binary.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <poll.h>

static int valid_dns_server(const char* server) {
    unsigned char address[16];
    if(inet_pton(AF_INET,server,address)==1 || inet_pton(AF_INET6,server,address)==1)return 1;
    const char* zone=strchr(server,'%');
    if(!zone || zone==server || !zone[1] || (size_t)(zone-server)>=INET6_ADDRSTRLEN)return 0;
    char base[INET6_ADDRSTRLEN];
    memcpy(base,server,(size_t)(zone-server));base[zone-server]=0;
    if(inet_pton(AF_INET6,base,address)!=1)return 0;
    for(const char* p=zone+1;*p;p++)
        if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'||*p=='.'))return 0;
    return strlen(zone+1)<16;
}

static int monitor_file(int directory, const char* name, const char* text) {
    int fd=openat(directory,name,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -1;
    size_t size=strlen(text);ssize_t written=write(fd,text,size);close(fd);
    return written==(ssize_t)size ? 0 : -1;
}
static int prepare_monitor(const char* display, const char* dns, char* path, size_t size) {
    snprintf(path,size,"%s-monitor",display);
    if(mkdir(path,0700) && errno!=EEXIST)return -1;
    int directory=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat info;
    if(directory<0)return -1;
    if(fstat(directory,&info)||info.st_uid!=getuid()||(info.st_mode&077)){close(directory);return -1;}
    char resolv[2048]={0};size_t used=0;
    char servers[2048];snprintf(servers,sizeof(servers),"%s",dns ? dns : "");
    char* state=NULL;
    for(char* server=strtok_r(servers,",",&state);server;server=strtok_r(NULL,",",&state)) {
        if(!valid_dns_server(server)) {close(directory);return -1;}
        int added=snprintf(resolv+used,sizeof(resolv)-used,"nameserver %s\n",server);
        if(added<0 || (size_t)added>=sizeof(resolv)-used){close(directory);return -1;}
        used+=(size_t)added;
    }
    int rc=monitor_file(directory,"resolv.conf",resolv) ||
        monitor_file(directory,"hosts","127.0.0.1 localhost\n::1 localhost\n") ||
        monitor_file(directory,"host.conf","multi on\n") || monitor_file(directory,"gai.conf","");
    close(directory);return rc ? -1 : 0;
}
static int start_session_bus(const char* display, const char* ref, const char* monitor) {
    char app[256], path[160], policy[192], address[192];
    size_t length=strcspn(ref,"/");
    if(length==0 || length>=sizeof(app))return -1;
    for(size_t i=0;i<length;i++) {
        char c=ref[i];
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return -1;
    }
    memcpy(app,ref,length);app[length]=0;
    snprintf(path,sizeof(path),"%s-bus",display);
    struct stat previous;
    if(!lstat(path,&previous)) {
        if(!S_ISSOCK(previous.st_mode)||previous.st_uid!=getuid()||unlink(path))return -1;
    } else if(errno!=ENOENT)return -1;
    snprintf(policy,sizeof(policy),"%s.policy",path);
    int fd=open(policy,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -1;
    char rule[512];int count=snprintf(rule,sizeof(rule),"own %s\ntalk org.freedesktop.portal.Flatpak\ntalk org.freedesktop.portal.Desktop\ntalk org.freedesktop.Flatpak\n",app);
    if(write(fd,rule,(size_t)count)!=count){close(fd);return -1;}close(fd);
    int ready[2];if(pipe2(ready,O_CLOEXEC)){unlink(policy);return -1;}
    pid_t parent=getpid(), broker=fork();
    if(broker<0){close(ready[0]);close(ready[1]);unlink(policy);return -1;}
    if(broker==0) {
        close(ready[0]);
        if(fcntl(ready[1],F_SETFD,0))_exit(127);
        char ready_fd[24];snprintf(ready_fd,sizeof(ready_fd),"%d",ready[1]);
        if(prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent)_exit(127);
        /* Broker stdout announces its address; it is not an application error. */
        int quiet=open("/dev/null",O_WRONLY|O_CLOEXEC);
        if(quiet<0||dup2(quiet,STDOUT_FILENO)<0)_exit(127);
        close(quiet);
        execl("/system_ext/bin/matonos-dbus-broker","matonos-dbus-broker",path,policy,"--flatpak-session",monitor,ready_fd,NULL);
        _exit(127);
    }
    close(ready[1]);
    struct pollfd poll_ready={.fd=ready[0],.events=POLLIN};char value=0;
    int success=poll(&poll_ready,1,5000)>0 && read(ready[0],&value,1)==1 && value==1;
    close(ready[0]);
    if(success) {
        snprintf(address,sizeof(address),"unix:path=%s",path);unlink(policy);
        if(setenv("DBUS_SESSION_BUS_ADDRESS",address,1)==0)return 0;
    }
    kill(broker,SIGTERM);while(waitpid(broker,NULL,0)<0&&errno==EINTR){}
    unlink(policy);unlink(path);return -1;
}

int main(int argc, char** argv) {
    const char* display = getenv("WAYLAND_DISPLAY");
    int display_fd = -1; char trailing; char display_copy[128] = {0};
    int graphical = display && sscanf(display, "/data/matonos/linux/runtime/wayland-%d%c", &display_fd, &trailing) == 1 && display_fd >= 0;
    if (graphical) snprintf(display_copy, sizeof(display_copy), "%s", display);
    char dns[2048], bus[192];
    snprintf(dns,sizeof(dns),"%s",getenv("MATON_FLATPAK_DNS") ? getenv("MATON_FLATPAK_DNS") : "");
    snprintf(bus,sizeof(bus),"%s",getenv("DBUS_SESSION_BUS_ADDRESS") ? getenv("DBUS_SESSION_BUS_ADDRESS") : "");
    if (clearenv() != 0 ||
        setenv("PATH", "/system_ext/bin:/system/bin:/system/xbin", 1) != 0 ||
        setenv("LD_LIBRARY_PATH", "/system_ext/lib64", 1) != 0 ||
        setenv("XDG_RUNTIME_DIR", "/data/matonos/linux/runtime", 1) != 0 ||
        setenv("TMPDIR", "/data/matonos/linux/cache", 1) != 0 ||
        setenv("HOME", "/data/matonos/linux/flatpak-data", 1) != 0 ||
        setenv("XDG_DATA_HOME", "/data/matonos/linux/flatpak-data/.local/share", 1) != 0 ||
        setenv("FLATPAK_SYSTEM_DIR", "/data/matonos/linux/flatpak", 1) != 0 ||
        setenv("FLATPAK_SYSTEM_CACHE_DIR", "/data/matonos/linux/cache", 1) != 0 ||
        setenv("FLATPAK_USER_DIR", "/data/matonos/linux/flatpak-user", 1) != 0 ||
        setenv("FLATPAK_DBUSPROXY", "/system_ext/bin/xdg-dbus-proxy", 1) != 0 ||
        setenv("FLATPAK_BWRAP", "/system_ext/bin/bwrap", 1) != 0) {
        perror("matonos-flatpak: setting runtime environment failed");
        return 127;
    }
    if (setenv("FLATPAK_REVOKEFS_FUSE", "/system_ext/bin/revokefs-fuse", 1) != 0) {
        perror("matonos-flatpak: setting revokefs path failed");
        return 127;
    }
    if (graphical && setenv("WAYLAND_DISPLAY", display_copy, 1) != 0) return 127;
    if (graphical) {
        if(setenv("MATON_FLATPAK_DNS",dns,1))return 127;
        char address[192];snprintf(address,sizeof(address),"unix:path=%s-bus",display_copy);
        struct stat socket_info;
        // Nested portal launches reuse the existing private bus and its services.
        if(!strcmp(bus,address) && !lstat(address+10,&socket_info) &&
                S_ISSOCK(socket_info.st_mode) && socket_info.st_uid==getuid()) {
            if(setenv("DBUS_SESSION_BUS_ADDRESS",bus,1))return 127;
        } else {
            char monitor[192];
            if(argc<2 || prepare_monitor(display_copy,dns,monitor,sizeof(monitor)) ||
                    start_session_bus(display_copy,argv[argc-1],monitor)) {
                fprintf(stderr,"matonos-flatpak: cannot start private session services\n");return 127;
            }
        }
    }
    execv("/system_ext/bin/matonos-flatpak", argv);
    perror("matonos-flatpak: exec failed");
    return 127;
}
