#define _GNU_SOURCE
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc,char **argv) {
    if(argc!=3)return 2;
    struct passwd *entry=getpwuid(getuid());
    if(!entry){perror("getpwuid");return 1;}
    if(strcmp(entry->pw_name,argv[1])||strcmp(entry->pw_dir,argv[2])) {
        fprintf(stderr,"unexpected account %s home %s\n",entry->pw_name,entry->pw_dir);
        return 1;
    }
    puts("PASS passwd map resolves the active UID and its configured home");
    return 0;
}
