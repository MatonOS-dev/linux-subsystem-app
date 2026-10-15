#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/*
 * Bionic management wrapper for the Flatpak CLI. linuxd execs
 * /apex/com.matonos.flatpak/bin/flatpak-env-wrapper; this process prepares
 * the management environment, then execs the static multicall
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
#include <stddef.h>

/* r24: the whole Flatpak stack ships in the com.matonos.flatpak APEX, mounted
 * at /apex/com.matonos.flatpak, instead of /system_ext. The binaries find
 * their own libraries through the APEX linker namespace, so no
 * LD_LIBRARY_PATH is needed. */
#define MATON_FLATPAK_APEX "/apex/com.matonos.flatpak"
#define MATON_FLATPAK_BIN MATON_FLATPAK_APEX "/bin"
/* An install operation's staging repo: /data/linux-app/staging/<uid>/<operation>. */
static int staging_directory_uid(const char* path, unsigned* result) {
    const char* last=path?strrchr(path,'/'):NULL;
    if(!last)return 0;
    char root[128];size_t len=(size_t)(last-path);
    if(len>=sizeof(root))return 0;
    memcpy(root,path,len);root[len]=0;
    const char* op=last+1;size_t op_len=strlen(op);
    if(!op_len || op_len>64)return 0;
    for(size_t i=0;i<op_len;i++){char c=op[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return 0;}
    return maton_parse_staging_owner(root,result);
}
static int install_directory_uid(const char* path, unsigned* result) {
    if(path && !strcmp(path,MATON_RUNTIME_DIR)){*result=1000;return 1;}
    if(geteuid()==1000 && maton_parse_pending_path(path)){*result=1000;return 1;}
    return maton_parse_install_path(path,result);
}

static int installer_staging_valid(const char* path) {
    char prefix[96];
    uid_t uid=geteuid();
    if(!maton_path_uid_valid((unsigned)uid))return 0;
    int n=snprintf(prefix,sizeof(prefix),MATON_STAGING_ROOT "/%u/", (unsigned)uid);
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
    char staging[160], home[192], data_home[224], config_home[224], cache[192], tmp[192], runtime[192];
    if(installer_mode) {
        snprintf(staging,sizeof(staging),"%s",staging_request);
        snprintf(home,sizeof(home),"%s/home",staging);
        snprintf(data_home,sizeof(data_home),"%s/home/.local/share",staging);
        snprintf(cache,sizeof(cache),"%s/cache",staging);
        snprintf(tmp,sizeof(tmp),"%s/tmp",staging);
        snprintf(runtime,sizeof(runtime),"%s/runtime",staging);
    } else {
        snprintf(home,sizeof(home),"/data/linux-app/manager/home");
        snprintf(data_home,sizeof(data_home),"/data/linux-app/manager/home/.local/share");
        snprintf(cache,sizeof(cache),"/data/linux-app/manager/cache");
        snprintf(tmp,sizeof(tmp),"/data/linux-app/manager/tmp");
        snprintf(runtime,sizeof(runtime),"/data/linux-app/manager/run");
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
         * the downloading app UID uses only its operation-owned scratch data. */
        setenv("FLATPAK_USER_DIR", installer_mode ? staging : flatpak_user_dir, 1) != 0 ||
        setenv("FLATPAK_DOWNLOAD_TMPDIR", tmp, 1) != 0 ||
        setenv("SSL_CERT_DIR", "/apex/com.android.conscrypt/cacerts", 1) != 0 ||
        setenv("CURL_CA_BUNDLE", "/apex/com.android.conscrypt/cacerts", 1) != 0 ||
        setenv("G_TLS_CA_PATH", "/apex/com.android.conscrypt/cacerts", 1) != 0) {
        perror("matonos-flatpak: setting runtime environment failed");
        return 127;
    }
    /* Host-side CLI work (remote-add's temporary GnuPG home, downloads) uses
     * the temp dir of the app whose installation it operates on:
     * /data/linux-app/install/<appid>-<version>/tmp, preferring the stub's --user
     * installation over the shared --system one. Android's /tmp is
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
        } else if(setenv("TMPDIR","/data/linux-app/manager/cache",1)||
                setenv("FLATPAK_DOWNLOAD_TMPDIR","/data/linux-app/manager/cache",1)) return 127;
    }
    execv(MATON_FLATPAK_BIN "/matonos-flatpak",argv);
    perror("matonos-flatpak: exec failed");return 127;
}
