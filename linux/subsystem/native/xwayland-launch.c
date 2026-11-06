/* APK entry point: the stock server gets its per-app extracted XKB data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <android/log.h>
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
    /* Diagnostics: Xwayland's stderr (client disconnect reasons) goes to
     * logcat as MatonXwayland, with -verbose raised. */
    int pipefd[2];
    if(!pipe(pipefd)){
        pid_t reader=fork();
        if(reader==0){
            close(pipefd[1]);
            FILE *in=fdopen(pipefd[0],"r");char line[1024];
            while(in && fgets(line,sizeof(line),in))__android_log_write(ANDROID_LOG_WARN,"MatonXwayland",line);
            _exit(0);
        }
        close(pipefd[0]);
        if(reader>0)dup2(pipefd[1],2);
        close(pipefd[1]);
    }
    char **args=calloc((size_t)argc+5,sizeof(*args));
    if(!args)return 1;
    args[0]=binary;
    for(int i=1;i<argc;i++)args[i]=argv[i];
    args[argc]="-xkbdir";args[argc+1]=(char *)data;
    args[argc+2]="-verbose";args[argc+3]="7";
    execv(binary,args);
    perror("Xwayland exec");
    return 1;
}
