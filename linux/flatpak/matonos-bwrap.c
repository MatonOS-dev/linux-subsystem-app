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
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BWRAP "/system_ext/bin/bwrap"
#define X11_SOCKET_PATH "/tmp/.X11-unix/X0"
#define X11_SOCKET_ENV "MATON_X11_SOCKET"
#define X11_DISPLAY ":0"
#define JOURNAL_SOCKET_ENV "MATON_JOURNAL_SOCKET"
#define JOURNAL_SOCKET_PATH "/run/systemd/journal/socket"

/* Values each bwrap option consumes, mirroring parse_args_recurse()
 * in bubblewrap's bubblewrap.c. Unknown dashed arguments count as
 * zero: bwrap rejects them before reaching the command anyway. */
static int option_values(const char* arg) {
    static const struct {
        const char* name;
        int values;
    } options[] = {
        /* two values */
        {"--bind",2},{"--bind-try",2},{"--ro-bind",2},{"--ro-bind-try",2},
        {"--dev-bind",2},{"--dev-bind-try",2},{"--bind-fd",2},{"--ro-bind-fd",2},
        {"--file",2},{"--bind-data",2},{"--ro-bind-data",2},{"--symlink",2},
        {"--chmod",2},{"--setenv",2},
        /* one value */
        {"--args",1},{"--argv0",1},{"--chdir",1},{"--remount-ro",1},
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

/* Build the argv passed to the real bwrap: the extra arguments go at
 * index at, everything else keeps its position. */
static char** insert_args(int argc, char** argv, int at, char** extra, int count) {
    char** extended=calloc((size_t)argc+(size_t)count+1,sizeof(char*));
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

int main(int argc, char** argv) {
    const char* socket_path;
    const char* journal_path;
    int args_end;
    int dashdash;
    int command;
    int at;
    char** extended;
    char* extra[21];
    int count=0;

    int x11_tmpfs=0, app_sandbox=0;
    char* bundled=NULL;
    char* bundled_journal=NULL;
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
    socket_path=getenv(X11_SOCKET_ENV);
    if(!socket_path || !*socket_path)socket_path=bundled;
    journal_path=getenv(JOURNAL_SOCKET_ENV);
    if(!journal_path || !*journal_path)journal_path=bundled_journal;
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
            execv(BWRAP,extended);
            perror("matonos-bwrap: exec failed");
            return 127;
        }
    }
    execv(BWRAP,argv);
    perror("matonos-bwrap: exec failed");
    return 127;
}
