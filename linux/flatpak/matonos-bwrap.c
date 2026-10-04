#define _GNU_SOURCE
/*
 * MatonOS bwrap shim. flatpak-env-wrapper.c routes the Flatpak
 * CLI through $FLATPAK_BWRAP, and flatpak run execs bwrap with
 * an empty environment: flatpak_bwrap_envp_to_args() turns the
 * whole launcher environment into --setenv arguments and
 * flatpak_bwrap_bundle_args() packs them, NUL-separated, into
 * the --args FD (flatpak-run.c / flatpak-bwrap.c). The per-app
 * X11 relay socket therefore arrives via the inherited
 * MATON_X11_SOCKET (direct bwrap callers keep the environment)
 * or, for flatpak run, via the --setenv MATON_X11_SOCKET
 * triplet inside the bundled FD data. Bind that socket onto the
 * /tmp/.X11-unix tmpfs flatpak mounts and restore DISPLAY,
 * then hand the real bwrap the extended argv.
 */
#include <glob.h>
#include "controller-access.h"
#include "maton-mount.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

/* r24: the Flatpak stack lives in the com.matonos.flatpak APEX. bwrap finds
 * its own libraries through the APEX linker namespace. */
#define MATON_FLATPAK_BIN "/apex/com.matonos.flatpak/bin"
#define BWRAP MATON_FLATPAK_BIN "/bwrap"
#define X11_SOCKET_PATH "/tmp/.X11-unix/X0"
#define X11_SOCKET_ENV "MATON_X11_SOCKET"
#define X11_DISPLAY ":0"
#define JOURNAL_SOCKET_ENV "MATON_JOURNAL_SOCKET"
#define JOURNAL_SOCKET_PATH "/run/systemd/journal/socket"
/* r24: bwrap runs matonos-app-exec as the sandbox command; it dyntransitions
 * to the verified app domain before exec'ing the payload. */
#define APP_EXEC_HOST MATON_FLATPAK_BIN "/matonos-app-exec"
#define APP_EXEC_SANDBOX "/run/matonos/matonos-app-exec"
#define APP_LABEL_ENV "MATON_APP_LABEL"

/* Values each bwrap option consumes, mirroring parse_args_recurse()
 * in bubblewrap's bubblewrap.c. Unknown dashed arguments count as
 * zero: bwrap rejects them before reaching the command anyway. */
static int option_values(const char* arg) {
    static const struct {
        const char* name;
        int values;
    } options[] = {
        /* bubblewrap 0.13.0: three values */
        {"--overlay",3},
        /* two values */
        {"--bind",2},{"--bind-try",2},{"--ro-bind",2},{"--ro-bind-try",2},
        {"--dev-bind",2},{"--dev-bind-try",2},{"--bind-fd",2},{"--ro-bind-fd",2},
        {"--file",2},{"--bind-data",2},{"--ro-bind-data",2},{"--symlink",2},
        {"--chmod",2},{"--setenv",2},
        /* one value */
        {"--args",1},{"--argv0",1},{"--chdir",1},{"--remount-ro",1},
        {"--overlay-src",1},{"--tmp-overlay",1},{"--ro-overlay",1},
        {"--proc",1},{"--exec-label",1},{"--file-label",1},{"--dev",1},
        {"--tmpfs",1},{"--mqueue",1},{"--dir",1},{"--lock-file",1},
        {"--sync-fd",1},{"--block-fd",1},{"--userns-block-fd",1},
        {"--info-fd",1},{"--json-status-fd",1},{"--seccomp",1},
        {"--add-seccomp-fd",1},{"--userns",1},{"--userns2",1},{"--pidns",1},
        {"--unsetenv",1},{"--uid",1},{"--gid",1},{"--hostname",1},
        {"--cap-add",1},{"--cap-drop",1},{"--perms",1},{"--size",1},
        /* every other option takes no values */
    };
    for(size_t i=0;i<sizeof(options)/sizeof(options[0]);i++)
        if(!strcmp(arg,options[i].name))return options[i].values;
    return 0;
}

/* Walk argv the way bwrap's parser does and report where option
 * parsing ends: the first non-option argument, or the argument
 * after an explicit "--", starts the command. *args_end is the
 * index just past the last "--args FD" pair, *dashdash the index
 * of the "--" itself; both stay -1 when absent. */
static int scan_argv(int argc, char** argv, int* args_end, int* dashdash) {
    int command=argc;
    *args_end=-1;
    *dashdash=-1;
    for(int i=1;i<argc;) {
        const char* arg=argv[i];
        if(!strcmp(arg,"--")) {
            *dashdash=i;
            command=i+1;
            break;
        }
        if(arg[0]!='-') {
            command=i;
            break;
        }
        if(!strcmp(arg,"--args") && i+1<argc)*args_end=i+2;
        i+=1+option_values(arg);
    }
    return command;
}

/* Start of the NUL-separated string following the one at p, or
 * NULL when p holds the last string in the buffer. */
static char* next_string(char* p, char* end) {
    char* nul=memchr(p,0,(size_t)(end-p));
    if(!nul || nul+1>=end)return NULL;
    return nul+1;
}

/* Recover the relay socket path from the bundled --args FD data:
 * flatpak converted MATON_X11_SOCKET into a --setenv VAR VALUE
 * triplet there. The FD is rewound to its original offset so the
 * real bwrap still reads the arguments from the start (bwrap's
 * load_file_data() reads from the current offset). */
static char* args_fd_x11_socket(int fd, int* x11_tmpfs, char** journal, int* app_sandbox) {
    char* data=NULL;
    size_t capacity=0, length=0;
    char *p,*end,*socket_path=NULL;
    off_t origin=lseek(fd,0,SEEK_CUR);
    if(origin==(off_t)-1)return NULL;
    for(;;) {
        ssize_t got;
        if(length==capacity) {
            size_t grown=capacity ? capacity*2 : 4096;
            char* bigger=realloc(data,grown);
            if(!bigger)break;
            data=bigger;
            capacity=grown;
        }
        do {
            got=read(fd,data+length,capacity-length);
        } while(got<0 && errno==EINTR);
        if(got<=0)break;
        length+=(size_t)got;
    }
    if(!data || !length || data[length-1]!=0) {
        (void)lseek(fd,origin,SEEK_SET);
        free(data);
        return NULL;
    }
    end=data+length;
    p=data;
    while(p && p<end) {
        char* var;
        char* value;
        if(p[0]!='-' || !strcmp(p,"--"))break;
        int values=option_values(p);
        if(!strcmp(p,"--setenv")) {
            var=next_string(p,end);
            value=var ? next_string(var,end) : NULL;
            if(var && !strcmp(var,X11_SOCKET_ENV) && value && *value)
                socket_path=value;
            if(var && !strcmp(var,JOURNAL_SOCKET_ENV) && value && *value)
                *journal=value;
            /* flatpak exports FLATPAK_ID only into the app sandbox. */
            if(var && !strcmp(var,"FLATPAK_ID"))*app_sandbox=1;
        } else if(!strcmp(p,"--tmpfs")) {
            value=next_string(p,end);
            if(value && !strcmp(value,"/tmp/.X11-unix"))*x11_tmpfs=1;
        }
        /* Values and command arguments must not masquerade as options. */
        for(int i=0;i<=values && p;i++)p=next_string(p,end);
    }
    if(lseek(fd,origin,SEEK_SET)==(off_t)-1)socket_path=NULL;
    return socket_path;
}

/* Look up a single --setenv value in the bundled --args FD data. The FD is
 * rewound so the real bwrap still reads it from the start (see above). */
static char* args_fd_lookup(int fd, const char* wanted) {
    char* data=NULL;
    size_t capacity=0, length=0;
    off_t origin=lseek(fd,0,SEEK_CUR);
    if(origin==(off_t)-1)return NULL;
    for(;;) {
        ssize_t got;
        if(length==capacity) {
            size_t grown=capacity ? capacity*2 : 4096;
            char* bigger=realloc(data,grown);
            if(!bigger)break;
            data=bigger;capacity=grown;
        }
        do { got=read(fd,data+length,capacity-length); } while(got<0 && errno==EINTR);
        if(got<=0)break;
        length+=(size_t)got;
    }
    char* found=NULL;
    if(data && length && data[length-1]==0) {
        char* end=data+length;
        char* p=data;
        while(p && p<end) {
            if(p[0]!='-' || !strcmp(p,"--"))break;
            int values=option_values(p);
            if(!strcmp(p,"--setenv")) {
                char* var=next_string(p,end);
                char* value=var ? next_string(var,end) : NULL;
                if(var && value && !strcmp(var,wanted) && *value) { free(found); found=strdup(value); }
            }
            for(int i=0;i<=values && p;i++)p=next_string(p,end);
        }
    }
    (void)lseek(fd,origin,SEEK_SET);
    free(data);
    return found;
}

/* Prepend one argument at index at. */
static char** prepend_argument(char** argv, int count, int at, const char* value) {
    char** out=calloc((size_t)count+2,sizeof(char*));
    if(!out)return NULL;
    for(int i=0;i<at;i++)out[i]=argv[i];
    out[at]=(char*)value;
    for(int i=at;i<count;i++)out[i+1]=argv[i];
    out[count+1]=NULL;
    return out;
}

/* Build the argv passed to the real bwrap: the extra arguments go at
 * index at, everything else keeps its position. */
static char** insert_args(int argc, char** argv, int at, char** extra, int count) {    char** extended=calloc((size_t)argc+(size_t)count+1,sizeof(char*));
    int i;
    int n=0;
    if(!extended)return NULL;
    for(i=0;i<at;i++)extended[n++]=argv[i];
    for(i=0;i<count;i++)extended[n++]=extra[i];
    for(i=at;i<argc;i++)extended[n++]=argv[i];
    return extended;
}
static int is_socket(const char* path) {
    struct stat info;
    return path && *path && lstat(path,&info)==0 && S_ISSOCK(info.st_mode);
}

/* Read the "child-pid" from the JSON bubblewrap writes to --info-fd. The
 * document is small; a bounded read and a scan for the field is enough. */
static int read_info_pid(int fd,pid_t* pid) {
    char buffer[4096];size_t used=0;
    for(;;) {
        if(used>=sizeof(buffer)-1)break;
        ssize_t got=read(fd,buffer+used,sizeof(buffer)-1-used);
        if(got<0) { if(errno==EINTR)continue; break; }
        if(got==0)break;
        used+=(size_t)got;
        if(memchr(buffer,'}',used))break;
    }
    buffer[used]=0;
    char* field=strstr(buffer,"\"child-pid\"");
    if(!field)return -1;
    char* colon=strchr(field,':');
    if(!colon)return -1;
    long value=strtol(colon+1,NULL,10);
    if(value<=0)return -1;
    *pid=(pid_t)value;
    return 0;
}

/* Ask the privileged mount helper to mount the verified images into the
 * sandbox's mount namespace. The rendezvous is the connected socketpair end
 * inherited from the launcher wrapper (never bound, so it has no name). We pass
 * a pidfd for the sandbox process - not a bare pid or namespace fd - and the
 * helper independently verifies it before opening the namespace itself. The
 * helper derives the app MLS level from our kernel-provided credentials and the
 * loop device from the installer's store attach record. */
static int send_mount_request(int mount_fd,const char* app_id,pid_t sandbox) {
    if(mount_fd<0 || !app_id || !*app_id)return 0;
    long pidfd=syscall(SYS_pidfd_open,sandbox,0);
    if(pidfd<0)return 0;
    struct maton_mount_request request;
    memset(&request,0,sizeof(request));
    request.magic=MATON_MOUNT_MAGIC;request.version=MATON_MOUNT_VERSION;
    snprintf(request.app_id,sizeof(request.app_id),"%s",app_id);
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec payload={.iov_base=&request,.iov_len=sizeof(request)};
    struct msghdr message={.msg_iov=&payload,.msg_iovlen=1,
        .msg_control=control,.msg_controllen=sizeof(control)};
    struct cmsghdr* rights=CMSG_FIRSTHDR(&message);
    rights->cmsg_level=SOL_SOCKET;rights->cmsg_type=SCM_RIGHTS;
    rights->cmsg_len=CMSG_LEN(sizeof(int));
    int fd=(int)pidfd;
    memcpy(CMSG_DATA(rights),&fd,sizeof(int));
    int ok=0;
    if(sendmsg(mount_fd,&message,MSG_NOSIGNAL)==(ssize_t)sizeof(request)) {
        struct maton_mount_reply reply;
        ssize_t got=recv(mount_fd,&reply,sizeof(reply),0);
        ok=got==(ssize_t)sizeof(reply) && reply.status==0;
        if(!ok && got==(ssize_t)sizeof(reply))
            fprintf(stderr,"matonos-bwrap: mount helper refused: %s\n",reply.error);
    }
    close((int)pidfd);
    return ok;
}

/* Run bubblewrap and, while it waits on --block-fd before running the payload,
 * have the privileged helper mount the app's verified images into the sandbox.
 * Any failure kills the sandbox (fail closed). Returns bubblewrap's status. */
static int launch_sandbox(char** argv,int info_write,int info_read,int block_read,
        int block_write,int mount_fd,const char* app_id) {
    pid_t child=fork();
    if(child<0) {
        perror("matonos-bwrap: fork");
        return 127;
    }
    if(child==0) {
        close(info_read);close(block_write);
        if(fcntl(info_write,F_SETFD,0)||fcntl(block_read,F_SETFD,0))_exit(127);
        execv(BWRAP,argv);
        _exit(127);
    }
    close(info_write);close(block_read);
    pid_t sandbox=0;
    int ok=read_info_pid(info_read,&sandbox)==0;
    close(info_read);
    if(ok)ok=send_mount_request(mount_fd,app_id,sandbox);
    if(ok) {
        char released=1;
        ok=write(block_write,&released,1)==1;
    }
    close(block_write);
    if(!ok) {
        fprintf(stderr,"matonos-bwrap: app code mount failed; killing sandbox\n");
        kill(child,SIGKILL);
        while(waitpid(child,NULL,0)<0&&errno==EINTR){}
        return 127;
    }
    int status=0;
    while(waitpid(child,&status,0)<0&&errno==EINTR){}
    return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}

int main(int argc, char** argv) {
    const char* socket_path;
    const char* journal_path;
    int args_end;
    int dashdash;
    int command;
    int at;
    char** extended;
    glob_t controllers = {0};
    char** extra;
    int count=0;

    int x11_tmpfs=0, app_sandbox=0;
    char* bundled=NULL;
    char* bundled_journal=NULL;
    char* app_label=NULL;
    char* app_id=NULL;
    char* mount_fd_text=NULL;
    int mount_fd=-1;
    int info_pipe[2]={-1,-1}, block_pipe[2]={-1,-1};
    int needs_mounts=0;
    static char info_fd_arg[16], block_fd_arg[16];
    command=scan_argv(argc,argv,&args_end,&dashdash);
    for(int i=1;i<command && strcmp(argv[i],"--");) {
        if(!strcmp(argv[i],"--setenv") && i+2<command && !strcmp(argv[i+1],"FLATPAK_ID"))app_sandbox=1;
        i+=1+option_values(argv[i]);
    }
    /* Only the app sandbox gets the binds: flatpak's own --tmpfs
     * /tmp/.X11-unix (from --socket=x11) marks X11 access, FLATPAK_ID
     * the app itself. Helper sandboxes such as xdg-dbus-proxy inherit
     * the variables but have neither and must stay untouched. */
    if(args_end>=0)bundled=args_fd_x11_socket(atoi(argv[args_end-1]),&x11_tmpfs,&bundled_journal,&app_sandbox);
    if(args_end>=0)app_label=args_fd_lookup(atoi(argv[args_end-1]),"MATON_APP_LABEL");
    if(!app_label && getenv("MATON_APP_LABEL"))app_label=strdup(getenv("MATON_APP_LABEL"));
    if(args_end>=0)app_id=args_fd_lookup(atoi(argv[args_end-1]),MATON_APP_ID_ENV);
    if(!app_id && getenv(MATON_APP_ID_ENV))app_id=strdup(getenv(MATON_APP_ID_ENV));
    /* r24: the privileged mount helper rendezvous. The launcher created an
     * unnamed socketpair and left our end inherited (not CLOEXEC); flatpak may
     * have turned the launcher environment into bundled --setenv triplets, so
     * look there too. A socketpair cannot be addressed by name, so no app can
     * connect to or race it. */
    if(args_end>=0)mount_fd_text=args_fd_lookup(atoi(argv[args_end-1]),MATON_MOUNT_FD_ENV);
    if(!mount_fd_text && getenv(MATON_MOUNT_FD_ENV))mount_fd_text=strdup(getenv(MATON_MOUNT_FD_ENV));
    if(mount_fd_text && *mount_fd_text)mount_fd=atoi(mount_fd_text);
    if(mount_fd>=0 && fcntl(mount_fd,F_GETFD)<0)mount_fd=-1;
    /* Only enter the app domain for the real app sandbox; helpers keep their
     * own flow. The label must be a matonos app context or it is ignored. */
    if(!app_sandbox || !app_label || strncmp(app_label,"u:r:matonos_flatpak_app",23)) {
        free(app_label);app_label=NULL;
    }
    /* A verified app sandbox is always set up by the privileged helper. If the
     * helper or its rendezvous or the verified app id is missing we fail
     * closed: never run a verified app unmounted. */
    needs_mounts=app_sandbox;
    if(needs_mounts && (mount_fd<0 || !app_id || !*app_id)) {
        fprintf(stderr,"matonos-bwrap: verified app launch without a mount helper\n");
        return 127;
    }
    if(needs_mounts) {
        if(pipe2(info_pipe,O_CLOEXEC)||pipe2(block_pipe,O_CLOEXEC)) {
            perror("matonos-bwrap: pipe");
            return 127;
        }
        snprintf(info_fd_arg,sizeof(info_fd_arg),"%d",info_pipe[1]);
        snprintf(block_fd_arg,sizeof(block_fd_arg),"%d",block_pipe[0]);
    }
    socket_path=getenv(X11_SOCKET_ENV);
    if(!socket_path || !*socket_path)socket_path=bundled;
    journal_path=getenv(JOURNAL_SOCKET_ENV);
    if(!journal_path || !*journal_path)journal_path=bundled_journal;
    /* Old Flatpak has no devices=input support. Add only controller nodes,
     * using the actual inherited group rather than sandbox environment data. */
    if (app_sandbox && controller_group_present())
        glob("/dev/hidraw*", GLOB_NOSORT, NULL, &controllers);
    /* uinput 3, X11 6, journal 3, machine-id 6, udev 3, SDL fallback 3,
     * app-exec bind/setenv 6, loop binds/setenvs + capability 18. */
    extra = calloc(56 + 3 * controllers.gl_pathc, sizeof(char*));
    if (!extra) return 127;
    if (app_sandbox && controller_group_present()) {
        for (size_t i = 0; i < controllers.gl_pathc; ++i) {
            struct stat node;
            if (lstat(controllers.gl_pathv[i], &node) || !S_ISCHR(node.st_mode)) continue;
            extra[count++] = "--dev-bind"; extra[count++] = controllers.gl_pathv[i]; extra[count++] = controllers.gl_pathv[i];
        }
        struct stat node;
        if (!lstat("/dev/uinput", &node) && S_ISCHR(node.st_mode)) {
            extra[count++] = "--dev-bind"; extra[count++] = "/dev/uinput"; extra[count++] = "/dev/uinput";
        }
    }
    if(x11_tmpfs && is_socket(socket_path)) {
        extra[count++]="--bind";extra[count++]=(char*)socket_path;extra[count++]=X11_SOCKET_PATH;
        extra[count++]="--setenv";extra[count++]="DISPLAY";extra[count++]=X11_DISPLAY;
    }
    /* Programs logging straight to journald (tracing-journald, sd_journal)
     * fail to start without its socket; the launcher drains this one into
     * the app's launch log. */
    if(app_sandbox && is_socket(journal_path)) {
        extra[count++]="--bind";extra[count++]=(char*)journal_path;extra[count++]=JOURNAL_SOCKET_PATH;
    }
    /* flatpak-run.c only uses the host ID if /etc or /var has one.
     * Android has neither. Override both paths after Flatpak mounts /var. */
    if(app_sandbox) {
        extra[count++]="--ro-bind";extra[count++]="/data/matonos/linux/machine-id";extra[count++]="/etc/machine-id";
        extra[count++]="--ro-bind";extra[count++]="/data/matonos/linux/machine-id";extra[count++]="/var/lib/dbus/machine-id";
        /* Flatpak already exposes /sys/class, /sys/dev and /sys/devices.
         * Bind the directory itself, so atomic database replacements and
         * future devices are visible in existing sandboxes. Metadata does
         * not grant access to any device node. */
        extra[count++]="--ro-bind";extra[count++]="/data/matonos/linux/udev";extra[count++]="/run/udev";
        /* Host udev multicast cannot reliably cross Flatpak's net namespace,
         * and linuxd's system UID is not a trusted root udev sender. SDL2/3
         * support this generic hint and watch /dev/input with inotify (or
         * poll when inotify is unavailable), including permission changes. */
        extra[count++]="--setenv";extra[count++]="SDL_JOYSTICK_DISABLE_UDEV";extra[count++]="1";
    }
    /* r24: expose the launcher and its label inside the app sandbox. bwrap
     * calls it as the command; it setcon()s to the app domain and execs the
     * verified payload. */
    if(app_sandbox && app_label) {
        extra[count++]="--ro-bind";extra[count++]=APP_EXEC_HOST;extra[count++]=APP_EXEC_SANDBOX;
        extra[count++]="--setenv";extra[count++]=APP_LABEL_ENV;extra[count++]=app_label;
    }
    /* r24: the app's verified images are mounted from *outside* the sandbox by
     * the privileged matonos-mount-helper. bubblewrap reports the sandbox
     * child pid on --info-fd and waits on --block-fd before it execs the
     * payload; the shim performs the helpers' round trip and only then
     * releases bubblewrap. No loop device, mount right or capability enters
     * the sandbox. */
    if(needs_mounts) {
        extra[count++]="--info-fd";extra[count++]=info_fd_arg;
        extra[count++]="--block-fd";extra[count++]=block_fd_arg;
    }
    if(count) {
        /* bwrap applies the bundled arguments at the --args pair,
         * so inserting right after it puts the bind on top of
         * flatpak's --tmpfs /tmp/.X11-unix and after its sorted
         * --setenv/--unsetenv environment arguments (our DISPLAY
         * therefore wins), while staying ahead of "--" and the
         * command. Without a bundled pair, insert where option
         * parsing ends: before an explicit "--", else directly
         * before the command. */
        at=args_end>=0 ? args_end : (dashdash>=0 ? dashdash : command);
        extended=insert_args(argc,argv,at,extra,count);
        if(extended) {
            unsetenv(X11_SOCKET_ENV);
            unsetenv(JOURNAL_SOCKET_ENV);
            char** final_argv=extended;
            if(app_label && command<argc) {
                /* The command was shifted right by the inserted arguments;
                 * put the launcher immediately in front of it. */
                int shifted=command + (at<=command ? count : 0);
                char** with_launcher=prepend_argument(extended,argc+count,shifted,APP_EXEC_SANDBOX);
                if(with_launcher) final_argv=with_launcher;
            }
            if(needs_mounts)
                return launch_sandbox(final_argv,info_pipe[1],info_pipe[0],
                        block_pipe[0],block_pipe[1],mount_fd,app_id);
            execv(BWRAP,final_argv);
            perror("matonos-bwrap: exec failed");
            return 127;
        }
    }
    execv(BWRAP,argv);
    perror("matonos-bwrap: exec failed");
    return 127;
}
