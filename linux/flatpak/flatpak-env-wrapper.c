#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/*
 * Static musl host launcher for the static Flatpak CLI. linuxd execs
 * /apex/com.matonos.flatpak/bin/flatpak-env-wrapper; this process prepares
 * the Android-side session and environment, then execs the static multicall
 * binary at matonos-flatpak. linuxd passes argv[0]="flatpak" or "ostree"
 * so multicall dispatch does not need APEX symlinks (Soong drops prebuilt
 * symlinks from the payload).
 */
#include "linux-paths.h"
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
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "machine-id.h"
#include <stddef.h>

/* r24: the whole Flatpak stack ships in the com.matonos.flatpak APEX, mounted
 * at /apex/com.matonos.flatpak, instead of /system_ext. The binaries find
 * their own libraries through the APEX linker namespace, so no
 * LD_LIBRARY_PATH is needed. */
#define MATON_FLATPAK_APEX "/apex/com.matonos.flatpak"
#define MATON_FLATPAK_BIN MATON_FLATPAK_APEX "/bin"
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

/* An install operation's staging repo: install/<uid>/staging/<operation>. */
static int staging_directory_uid(const char* path, unsigned* result) {
    const char* marker=path?strstr(path,"/staging/"):NULL;
    if(!marker)return 0;
    char root[128];size_t len=(size_t)(marker-path);
    if(len>=sizeof(root))return 0;
    memcpy(root,path,len);root[len]=0;
    const char* op=marker+9;size_t op_len=strlen(op);
    if(!op_len || op_len>64)return 0;
    for(size_t i=0;i<op_len;i++){char c=op[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return 0;}
    return maton_parse_install_path(root,result);
}
static int install_directory_uid(const char* path, unsigned* result) {
    return maton_parse_install_path(path,result) || maton_parse_path(path,"de","runtime",result);
}

static int monitor_file(int directory, const char* name, const char* text) {
    int fd=openat(directory,name,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0644);
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
    int server_count=0;
    for(char* server=strtok_r(servers,",",&state);server;server=strtok_r(NULL,",",&state)) {
        if(!valid_dns_server(server)) {close(directory);return -1;}
        int added=snprintf(resolv+used,sizeof(resolv)-used,"nameserver %s\n",server);
        if(added<0 || (size_t)added>=sizeof(resolv)-used){close(directory);return -1;}
        used+=(size_t)added;
        server_count++;
    }
    /* Phase 1 stub: linuxd will replace loopback with its per-app forwarder. */
    if(!server_count)snprintf(resolv,sizeof(resolv),"nameserver 127.0.0.1\n");
    int rc=monitor_file(directory,"resolv.conf",resolv) ||
        monitor_file(directory,"hosts","127.0.0.1 localhost\n::1 localhost\n") ||
        monitor_file(directory,"host.conf","multi on\n") || monitor_file(directory,"gai.conf","") ||
        monitor_file(directory,"passwd","linuxd:x:1000:1000:Linux user:/home/linuxd:/sbin/nologin\n") ||
        monitor_file(directory,"group","linuxd:x:1000:\n");
    close(directory);return rc ? -1 : 0;
}
static int installer_staging_valid(const char* path) {
    char prefix[96];
    uid_t uid=geteuid();
    if(uid<10000 || uid%100000<10000 || uid%100000>19999)return 0;
    int n=snprintf(prefix,sizeof(prefix),MATON_DE_ROOT_FORMAT "/install/%u/staging/", maton_user_of((unsigned)uid), (unsigned)uid);
    if(n<0 || n>=(int)sizeof(prefix) || strncmp(path,prefix,(size_t)n))return 0;
    const char* op=path+n;
    size_t len=strlen(op);
    if(!len || len>64)return 0;
    for(size_t i=0;i<len;i++){char c=op[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return 0;}
    struct stat st;
    return !lstat(path,&st) && S_ISDIR(st.st_mode) && st.st_uid==uid;
}

/* Install/query operations only. App launch belongs exclusively to the stub. */
int main(int argc,char **argv) {
    if(strcmp(argv[0],"flatpak") && strcmp(argv[0],"ostree"))return 127;
    for(int i=1;i<argc;i++)if(!strcmp(argv[i],"run")) {
        fprintf(stderr,"matonos-flatpak: launch from the signed stub is required\n");return 127;
    }
    const char *staging_request=getenv("MATON_FLATPAK_STAGING_DIR");
    int installer_mode=staging_request && installer_staging_valid(staging_request);
    if(staging_request && !installer_mode)return 127;
    const char *supplied_system_dir=getenv("FLATPAK_SYSTEM_DIR");
    const char *supplied_user_dir=getenv("FLATPAK_USER_DIR");
    unsigned install_uid;
    if(!installer_mode && ((!supplied_system_dir && !supplied_user_dir) ||
       (supplied_system_dir && !install_directory_uid(supplied_system_dir,&install_uid) &&
            !staging_directory_uid(supplied_system_dir,&install_uid)) ||
       (supplied_user_dir && !install_directory_uid(supplied_user_dir,&install_uid) &&
            !staging_directory_uid(supplied_user_dir,&install_uid))))return 127;
    char flatpak_system_dir[128],flatpak_user_dir[128];
    snprintf(flatpak_system_dir,sizeof(flatpak_system_dir),"%s",supplied_system_dir?supplied_system_dir:supplied_user_dir?supplied_user_dir:staging_request?staging_request:"");
    snprintf(flatpak_user_dir,sizeof(flatpak_user_dir),"%s",supplied_user_dir?supplied_user_dir:supplied_system_dir?supplied_system_dir:staging_request?staging_request:"");
    char static_config[192]={0},static_base[192];
    if(!installer_mode) {
        char reason[256];
        if(prepare_machine_id_reason("/data/matonos/linux",reason,sizeof(reason)))return 127;
        snprintf(static_base,sizeof(static_base),"/data/matonos/linux/run/flatpak-config-%d",getpid());
        if(prepare_monitor(static_base,getenv("MATON_FLATPAK_DNS"),static_config,sizeof(static_config)))return 127;
    }
    char staging[160], home[192], data_home[224], config_home[224], cache[192], tmp[192], runtime[192];
    if(installer_mode) {
        snprintf(staging,sizeof(staging),"%s",staging_request);
        snprintf(home,sizeof(home),"%s/home",staging);
        snprintf(data_home,sizeof(data_home),"%s/home/.local/share",staging);
        snprintf(cache,sizeof(cache),"%s/cache",staging);
        snprintf(tmp,sizeof(tmp),"%s/tmp",staging);
        snprintf(runtime,sizeof(runtime),"%s/runtime",staging);
    } else {
        snprintf(home,sizeof(home),"/data/matonos/linux/flatpak-data");
        snprintf(data_home,sizeof(data_home),"/data/matonos/linux/flatpak-data/.local/share");
        snprintf(cache,sizeof(cache),"/data/matonos/linux/cache");
        snprintf(tmp,sizeof(tmp),"/tmp");
        snprintf(runtime,sizeof(runtime),"/data/matonos/linux/run");
    }
    snprintf(config_home,sizeof(config_home),"%s/.config",home);
    if (clearenv() != 0 ||
        setenv("PATH", MATON_FLATPAK_BIN ":/system/bin:/system/xbin", 1) != 0 ||
        setenv("XDG_RUNTIME_DIR", runtime, 1) != 0 ||
        setenv("HOME", home, 1) != 0 ||
        setenv("TMPDIR", tmp, 1) != 0 ||
        setenv("XDG_DATA_HOME", data_home, 1) != 0 ||
        setenv("XDG_CONFIG_HOME", config_home, 1) != 0 ||
        setenv("XDG_CACHE_HOME", cache, 1) != 0 ||
        setenv("FLATPAK_SYSTEM_DIR", installer_mode ? staging : flatpak_system_dir, 1) != 0 ||
        setenv("FLATPAK_SYSTEM_CACHE_DIR", cache, 1) != 0 ||
        /* Flatpak checks this directory for extra-data even on --system
         * installs. Keep that lookup inside the verified operation tree:
         * the downloading app UID cannot traverse the legacy shared root. */
        setenv("FLATPAK_USER_DIR", installer_mode ? staging : flatpak_user_dir, 1) != 0 ||
        setenv("FLATPAK_DOWNLOAD_TMPDIR", tmp, 1) != 0 ||
        setenv("SSL_CERT_DIR", "/apex/com.android.conscrypt/cacerts", 1) != 0 ||
        setenv("CURL_CA_BUNDLE", "/apex/com.android.conscrypt/cacerts", 1) != 0 ||
        setenv("G_TLS_CA_PATH", "/apex/com.android.conscrypt/cacerts", 1) != 0 ||
        setenv("MATON_FLATPAK_CONFIG_DIR", static_config, 1) != 0) {
        perror("matonos-flatpak: setting runtime environment failed");
        return 127;
    }
    /* Host-side CLI work (remote-add's temporary GnuPG home, downloads) uses
     * the temp dir of the app whose installation it operates on:
     * /data/misc_de/<u>/matonos-linux/install/<uid>/tmp, preferring the stub's --user
     * installation over the runtime app's --system one. Android's /tmp is
     * shell-owned and not writable for us. Staging keeps its own tmp. */
    if(!installer_mode) {
        unsigned tmp_uid=0;
        const char* root=install_directory_uid(flatpak_user_dir,&tmp_uid) ? flatpak_user_dir :
                install_directory_uid(flatpak_system_dir,&tmp_uid) ? flatpak_system_dir : NULL;
        struct stat root_info;
        /* linuxd creates (and labels) installation roots. Before the first
         * operation on one, use linuxd's own temp dir instead. */
        if(root && stat(root,&root_info)==0 && S_ISDIR(root_info.st_mode)) {
            char app_tmp[160];
            snprintf(app_tmp,sizeof(app_tmp),"%s/tmp",root);
            if(mkdir(app_tmp,0700) && errno!=EEXIST) {
                perror("matonos-flatpak: app temp dir");return 127;
            }
            if(setenv("TMPDIR",app_tmp,1)||setenv("FLATPAK_DOWNLOAD_TMPDIR",app_tmp,1))return 127;
        } else if(setenv("TMPDIR","/data/matonos/linux/cache",1)||
                setenv("FLATPAK_DOWNLOAD_TMPDIR","/data/matonos/linux/cache",1)) return 127;
    }
    execv(MATON_FLATPAK_BIN "/matonos-flatpak",argv);
    perror("matonos-flatpak: exec failed");return 127;
}
