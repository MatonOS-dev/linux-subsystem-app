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
#include <time.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "machine-id.h"
#include "controller-access.h"
#include "../dbus-broker/session-control.h"

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
/* A journald socket for the app sandbox (matonos-bwrap binds it at
 * /run/systemd/journal/socket): programs built with journald logging refuse
 * to start without one. Each datagram's MESSAGE field goes to stderr, which
 * linuxd keeps as the app's launch log. */
static int start_journal_sink(const char* display, char* path, size_t size) {
    struct sockaddr_un address={.sun_family=AF_UNIX};
    snprintf(path,size,"%s-journal",display);
    if(strlen(path)>=sizeof(address.sun_path))return -1;
    struct stat previous;
    if(!lstat(path,&previous)) {
        if(!S_ISSOCK(previous.st_mode)||previous.st_uid!=getuid()||unlink(path))return -1;
    } else if(errno!=ENOENT)return -1;
    strcpy(address.sun_path,path);
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);
    if(fd<0)return -1;
    if(bind(fd,(struct sockaddr*)&address,sizeof(address))){close(fd);return -1;}
    pid_t parent=getpid(), sink=fork();
    if(sink<0){close(fd);unlink(path);return -1;}
    if(sink>0){close(fd);return 0;}
    if(prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent)_exit(0);
    const char* directory=getenv("MATON_SESSION_DIRECTORY_FD");
    if(directory)close(atoi(directory));
    const char* x11_directory=getenv("MATON_SESSION_X11_FD");
    if(x11_directory)close(atoi(x11_directory));
    static char message[65536];
    for(;;) {
        ssize_t got=recv(fd,message,sizeof(message)-1,0);
        if(got<0){if(errno==EINTR)continue;break;}
        message[got]=0;
        /* Native protocol: KEY=value lines; binary fields are skipped. */
        for(char* line=message;line<message+got;) {
            char* end=memchr(line,'\n',(size_t)(message+got-line));
            if(!end)end=message+got;
            if(end-line>8&&!memcmp(line,"MESSAGE=",8)) {
                (void)!write(STDERR_FILENO,line+8,(size_t)(end-line-8));
                (void)!write(STDERR_FILENO,"\n",1);
            }
            line=end+1;
        }
    }
    unlink(path);_exit(0);
}
/* Query the actual image CLI, bypassing this wrapper (no recursive portal
 * startup). Bounded output/time; a broken CLI reports unknown and fails closed. */
static void system_flatpak_version(char version[64]) {
    snprintf(version,64,"unknown");
    int output[2];if(pipe2(output,O_CLOEXEC))return;
    pid_t child=fork();
    if(child<0){close(output[0]);close(output[1]);return;}
    if(child==0) {
        close(output[0]);
        if(dup2(output[1],STDOUT_FILENO)<0)_exit(127);
        close(output[1]);
        execl("/system_ext/bin/matonos-flatpak","flatpak","--version",NULL);
        _exit(127);
    }
    close(output[1]);
    if(fcntl(output[0],F_SETFL,O_NONBLOCK)<0) {
        close(output[0]);kill(child,SIGKILL);while(waitpid(child,NULL,0)<0&&errno==EINTR){};return;
    }
    char text[128];size_t used=0;int status=0,exited=0;
    struct timespec start,now;clock_gettime(CLOCK_MONOTONIC,&start);
    for(;;) {
        ssize_t got=read(output[0],text+used,sizeof(text)-1-used);
        if(got>0)used+=(size_t)got;
        pid_t waited=waitpid(child,&status,WNOHANG);
        if(waited==child){exited=1;break;}
        if(waited<0 && errno!=EINTR)break;
        clock_gettime(CLOCK_MONOTONIC,&now);
        if((now.tv_sec-start.tv_sec)*1000+(now.tv_nsec-start.tv_nsec)/1000000>=2000 ||
           used==sizeof(text)-1)break;
        struct pollfd ready={.fd=got==0 ? -1 : output[0],.events=POLLIN};
        (void)poll(&ready,1,20);
    }
    if(exited && used<sizeof(text)-1) {
        ssize_t got=read(output[0],text+used,sizeof(text)-1-used);
        if(got>0)used+=(size_t)got;
    }
    close(output[0]);
    if(!exited){kill(child,SIGKILL);while(waitpid(child,NULL,0)<0&&errno==EINTR){};return;}
    if(!WIFEXITED(status) || WEXITSTATUS(status)!=0)return;
    text[used]=0;
    while(used && (text[used-1]=='\n'||text[used-1]=='\r'||text[used-1]==' '))text[--used]=0;
    if(strncmp(text,"Flatpak ",8) || !text[8] || strlen(text+8)>=64)return;
    snprintf(version,64,"%s",text+8);
}
/* The supervisor holds the delegated directory for the whole host session.
 * Only its bus socket path is exported; the control capability never reaches
 * the CLI, bwrap, portal, or application children. */
static int start_session_portal(int directory, int x11_directory, const char* x11_name, const char* monitor, char* bus, size_t size) {
    int ready[2];if(pipe2(ready,O_CLOEXEC))return -1;
    pid_t supervisor=fork();
    if(supervisor<0){close(ready[0]);close(ready[1]);return -1;}
    if(supervisor==0) {
        close(ready[0]);
        char flatpak_version[64];system_flatpak_version(flatpak_version);
        struct sockaddr_un address={.sun_family=AF_UNIX};
        snprintf(address.sun_path,sizeof(address.sun_path),"/proc/self/fd/%d/bus-control",directory);
        int control=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
        if(control<0 || connect(control,(struct sockaddr*)&address,sizeof(address)))_exit(127);
        int gate[2];if(pipe2(gate,O_CLOEXEC))_exit(127);
        pid_t parent=getpid(),portal=fork();
        if(portal<0)_exit(127);
        if(portal==0) {
            close(gate[1]);close(control);close(ready[1]);close(directory);
            if(x11_directory>=0)close(x11_directory);
            if(prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent)_exit(127);
            char value=0;if(read(gate[0],&value,1)!=1 || value!=1)_exit(127);
            close(gate[0]);
            char address[192];snprintf(address,sizeof(address),"unix:path=/proc/%d/fd/%d/bus",parent,directory);
            if(setenv("DBUS_SESSION_BUS_ADDRESS",address,1))_exit(127);
            snprintf(address,sizeof(address),"/proc/%d/fd/%d/wayland-0",parent,directory);
            if(setenv("WAYLAND_DISPLAY",address,1))_exit(127);
            if(x11_directory>=0) {
                snprintf(address,sizeof(address),"/proc/%d/fd/%d/%s",parent,x11_directory,x11_name);
                if(setenv("MATON_X11_SOCKET",address,1))_exit(127);
            } else unsetenv("MATON_X11_SOCKET");
            execl("/system_ext/bin/flatpak-portal","flatpak-portal",NULL);_exit(127);
        }
        close(gate[0]);
        struct MatonSessionRegistration registration={.pid=portal};
        snprintf(registration.flatpak_version,sizeof(registration.flatpak_version),"%s",flatpak_version);
        snprintf(registration.monitor,sizeof(registration.monitor),"%s",monitor);
        struct MatonSessionReply reply={0};
        struct pollfd event={.fd=control,.events=POLLIN};
        int ok=send(control,&registration,sizeof(registration),MSG_NOSIGNAL)==sizeof(registration) &&
            poll(&event,1,5000)>0 && recv(control,&reply,sizeof(reply),MSG_TRUNC)==sizeof(reply);
        if(ok && reply.status==MATON_SESSION_FLATPAK_UNSUPPORTED) {
            fprintf(stderr,"matonos-flatpak: %.*s: %.*s\n",
                (int)sizeof(reply.error),reply.error,(int)sizeof(reply.message),reply.message);
            ok=0;
        } else if(!ok) {
            fprintf(stderr,"matonos-flatpak: invalid or missing session portal registration reply\n");
        }
        if(ok && reply.status==0) {
            char value=1;ok=write(gate[1],&value,1)==1;
            if(ok)ok=poll(&event,1,5000)>0 && recv(control,&value,1,0)==1 && value==1;
        } else if(ok && reply.status==2) {
            close(gate[1]);gate[1]=-1;
            kill(portal,SIGTERM);while(waitpid(portal,NULL,0)<0&&errno==EINTR){}
            (void)!write(ready[1],&reply.supervisor,sizeof(reply.supervisor));_exit(0);
        } else ok=0;
        if(gate[1]>=0)close(gate[1]);
        if(ok)(void)!write(ready[1],&parent,sizeof(parent));
        close(ready[1]);
        /* Do not reap until control EOF has revoked the registered PID. A
         * zombie reserves the PID against reuse during broker revocation. */
        while(ok) {
            siginfo_t info={0};
            if(waitid(P_PID,portal,&info,WEXITED|WNOHANG|WNOWAIT)<0 || info.si_pid)break;
            int rc=poll(&event,1,100);
            if(rc<0 && errno==EINTR)continue;
            if(rc!=0)break;
        }
        kill(portal,SIGTERM);
        /* Closing control before reaping reserves the child PID until the
         * broker acknowledges revocation by closing its end. */
        shutdown(control,SHUT_WR);
        char byte;while(recv(control,&byte,1,0)<0&&errno==EINTR){}
        close(control);
        while(waitpid(portal,NULL,0)<0&&errno==EINTR){}
        close(directory);if(x11_directory>=0)close(x11_directory);_exit(ok?0:127);
    }
    close(ready[1]);pid_t owner=0;
    struct pollfd event={.fd=ready[0],.events=POLLIN};
    int ok=poll(&event,1,12000)>0 && read(ready[0],&owner,sizeof(owner))==sizeof(owner) && owner>0;
    close(ready[0]);
    if(!ok){kill(supervisor,SIGTERM);while(waitpid(supervisor,NULL,0)<0&&errno==EINTR){}return -1;}
    snprintf(bus,size,"unix:path=/proc/%d/fd/%d/bus",owner,directory);return 0;
}

int main(int argc, char** argv) {
    (void)argc;
    if (configure_controller_group()) { perror("matonos-flatpak: controller groups"); return 127; }
    const char* display = getenv("WAYLAND_DISPLAY");
    const char* bwrap = "/system_ext/bin/matonos-bwrap";
    if(prepare_machine_id("/data/matonos/linux")) {
        perror("matonos-flatpak: machine-id");return 127;
    }
    int display_fd = -1; char trailing; char display_copy[128] = {0};
    char x11_socket[160] = {0};
    int graphical = display && sscanf(display, "/data/matonos/linux/runtime/wayland-%d%c", &display_fd, &trailing) == 1 && display_fd >= 0;
    int nested=0, owner=0;
    if(!graphical && display && sscanf(display,"/proc/%d/fd/198/wayland-0%c",&owner,&trailing)==1 && owner>0) {
        char expected[192];snprintf(expected,sizeof(expected),"unix:path=/proc/%d/fd/198/bus",owner);
        nested=getenv("DBUS_SESSION_BUS_ADDRESS") && !strcmp(getenv("DBUS_SESSION_BUS_ADDRESS"),expected);
        snprintf(expected,sizeof(expected),"/proc/%d/fd/198/wayland-0",owner);
        nested=nested && !strcmp(display,expected);
        graphical=nested;
    }
    if (graphical) snprintf(display_copy, sizeof(display_copy), "%s", display);
    if (graphical) {
        // linuxd relays X11 on a sibling socket of the Wayland
        // one; only then does the bwrap shim have a socket to bind.
        struct stat socket_info;
        if(nested) {
            const char* inherited=getenv("MATON_X11_SOCKET");
            int pid,number;char extra;
            if(inherited && sscanf(inherited,"/proc/%d/fd/199/X%d%c",&pid,&number,&extra)==2 && pid==owner && number>=0)
                snprintf(x11_socket,sizeof(x11_socket),"%s",inherited);
        } else snprintf(x11_socket, sizeof(x11_socket), "%s-x11", display_copy);
        if (lstat(x11_socket, &socket_info) != 0 || !S_ISSOCK(socket_info.st_mode))
            x11_socket[0] = '\0';
        else
            bwrap = "/system_ext/bin/matonos-bwrap";
    }
    char journal[160] = {0};
    if (graphical) {
        char journal_display[128];
        snprintf(journal_display,sizeof(journal_display),"/data/matonos/linux/runtime/wayland-%d",getpid());
        if (start_journal_sink(nested?journal_display:display_copy, journal, sizeof(journal)) == 0)
            bwrap = "/system_ext/bin/matonos-bwrap";
        else journal[0] = '\0';
    }
    int session_directory=-1;
    const char* directory=getenv("MATON_SESSION_DIRECTORY_FD");
    if(directory) {
        if(strcmp(directory,"198"))return 127;
        session_directory=198;
        if(fcntl(session_directory,F_SETFD,FD_CLOEXEC))return 127;
    }
    int x11_directory=-1;char x11_name[64]={0};
    const char* x11_fd=getenv("MATON_SESSION_X11_FD");
    const char* x11_file=getenv("MATON_SESSION_X11_NAME");
    if(x11_fd) {
        int number;char extra;
        if(session_directory<0 || strcmp(x11_fd,"199") || !x11_file ||
           sscanf(x11_file,"X%d%c",&number,&extra)!=1 || number<0)return 127;
        x11_directory=199;
        if(fcntl(x11_directory,F_SETFD,FD_CLOEXEC))return 127;
        snprintf(x11_name,sizeof(x11_name),"%s",x11_file);
    }
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
        setenv("FLATPAK_DBUSPROXY", "/system_ext/bin/xdg-dbus-proxy", 1) != 0) {
        perror("matonos-flatpak: setting runtime environment failed");
        return 127;
    }
    // clearenv() above dropped everything, so the shim path
    // and its socket are exported here, after the reset.
    if (setenv("FLATPAK_BWRAP", bwrap, 1) != 0 ||
        (x11_socket[0] != '\0' && setenv("MATON_X11_SOCKET", x11_socket, 1) != 0) ||
        (journal[0] != '\0' && setenv("MATON_JOURNAL_SOCKET", journal, 1) != 0)) {
        perror("matonos-flatpak: setting bwrap environment failed");
        return 127;
    }
    if (setenv("FLATPAK_REVOKEFS_FUSE", "/system_ext/bin/revokefs-fuse", 1) != 0) {
        perror("matonos-flatpak: setting revokefs path failed");
        return 127;
    }
    if (graphical && setenv("WAYLAND_DISPLAY", display_copy, 1) != 0) return 127;
    if (graphical) {
        if(setenv("MATON_FLATPAK_DNS",dns,1))return 127;
        // Nested native portal launches already carry the host session bus.
        if(session_directory>=0) {
            char monitor[192];
            if(prepare_monitor(display_copy,dns,monitor,sizeof(monitor)) ||
               start_session_portal(session_directory,x11_directory,x11_name,monitor,bus,sizeof(bus))) {
                fprintf(stderr,"matonos-flatpak: cannot start session portal\n");return 127;
            }
        } else if(!bus[0]) {
            fprintf(stderr,"matonos-flatpak: missing compositor session bus\n");return 127;
        }
        if(setenv("DBUS_SESSION_BUS_ADDRESS",bus,1))return 127;
    }
    if(session_directory>=0)close(session_directory);
    if(x11_directory>=0)close(x11_directory);
    execv("/system_ext/bin/matonos-flatpak", argv);
    perror("matonos-flatpak: exec failed");
    return 127;
}
