/* APK entry point: the stock server gets its per-app extracted XKB data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
int main(int argc, char **argv) {
    const char *data=getenv("MATON_XKB_ROOT");
    const char *bin=getenv("MATON_XKB_BIN");
    if(!bin || chdir(bin))return 1;
    char binary[PATH_MAX];
    ssize_t n=readlink("/proc/self/exe",binary,sizeof(binary)-1);
    if(!data || n<0 || n>=(ssize_t)sizeof(binary)-1)return 1;
    binary[n]=0;
    char *slash=strrchr(binary,'/');
    if(!slash || (size_t)(slash-binary)+sizeof("/libmatonos-Xwayland-server.so")>sizeof(binary))return 1;
    strcpy(slash,"/libmatonos-Xwayland-server.so");
    char **args=calloc((size_t)argc+3,sizeof(*args));
    if(!args)return 1;
    args[0]=binary;
    for(int i=1;i<argc;i++)args[i]=argv[i];
    args[argc]="-xkbdir";args[argc+1]=(char *)data;
    execv(binary,args);
    perror("Xwayland exec");
    return 1;
}
