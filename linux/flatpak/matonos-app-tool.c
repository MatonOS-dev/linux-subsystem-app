#define _GNU_SOURCE
/* Unprivileged applet entry for the per-app stock portal. Every process
 * remains at the app UID, in untrusted_app, with its inherited seccomp
 * filter. This entry stays entirely under the app's identity. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

int main(int argc, char** argv) {
    (void)argc;
    unsigned uid=getuid();
    if(uid%100000<10000 || uid%100000>=20000 || geteuid()!=uid) {
        fprintf(stderr,"matonos-app-tool: app UID required\n");return 127;
    }
    const char* name=strrchr(argv[0],'/');name=name?name+1:argv[0];
    int bwrap=!strcmp(name,"matonos-app-bwrap") || !strcmp(name,"libmatonos-app-bwrap.so");
    const char* slash=strrchr(argv[0],'/');
    if(!slash){fprintf(stderr,"matonos-app-tool: missing executable directory\n");return 127;}
    size_t dir_len=(size_t)(slash-argv[0]);
    char flatpak[PATH_MAX],bwrap_path[PATH_MAX];
    if(dir_len+sizeof("/libmatonos-flatpak.so")>sizeof(flatpak) ||
       dir_len+sizeof("/libmatonos-app-bwrap.so")>sizeof(bwrap_path))return 127;
    memcpy(flatpak,argv[0],dir_len);strcpy(flatpak+dir_len,"/libmatonos-flatpak.so");
    memcpy(bwrap_path,argv[0],dir_len);strcpy(bwrap_path+dir_len,"/libmatonos-app-bwrap.so");
    /* The stock portal builds a minimal CLI environment and does not retain
     * FLATPAK_BWRAP. Pin the unprivileged applet for its nested run here. */
    if(!bwrap && setenv("FLATPAK_BWRAP",bwrap_path,1))return 127;
    /* exec is intercepted by the resident v3 loader; no SELinux transition,
     * identity switch or privileged shim is involved. */
    argv[0]=bwrap?"bwrap":"flatpak";
    execv(flatpak,argv);
    perror("matonos-app-tool: exec");return 127;
}
