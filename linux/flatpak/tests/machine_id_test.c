#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../machine-id.h"

static void read_id(int dir,char text[34]) {
    int fd=openat(dir,"machine-id",O_RDONLY|O_NOFOLLOW);assert(fd>=0);
    assert(read(fd,text,34)==33);text[33]=0;close(fd);
    assert(text[32]=='\n');
    for(int i=0;i<32;i++)assert((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f'));
}
int main(void) {
    char root[]="/tmp/maton-machine-id-XXXXXX";assert(mkdtemp(root));
    int dir=open(root,O_RDONLY|O_DIRECTORY);assert(dir>=0);
    /* Recover a crash between chmod/fsync and publishing the new ID. */
    int fd=openat(dir,"machine-id.tmp",O_CREAT|O_WRONLY,0444);assert(fd>=0);close(fd);
    pid_t children[8];
    for(unsigned i=0;i<8;i++) {
        children[i]=fork();assert(children[i]>=0);
        if(children[i]==0)_exit(prepare_machine_id(root)==0 ? 0 : 1);
    }
    for(unsigned i=0;i<8;i++){int status;assert(waitpid(children[i],&status,0)==children[i]);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
    char before[34],after[34];read_id(dir,before);
    for(unsigned i=0;i<10;i++)assert(prepare_machine_id(root)==0);
    read_id(dir,after);assert(!strcmp(before,after));
    struct stat st;assert(fstatat(dir,"machine-id",&st,0)==0);assert((st.st_mode&0777)==0444);
    assert(fchmodat(dir,"machine-id",0600,0)==0);
    fd=openat(dir,"machine-id",O_WRONLY|O_TRUNC);assert(fd>=0);
    assert(write(fd,"00000000000000000000000000000000\n",33)==33);close(fd);
    assert(prepare_machine_id(root)!=0); /* never silently replace an invalid stored ID */
    assert(unlinkat(dir,"machine-id",0)==0);assert(symlinkat("/dev/null",dir,"machine-id")==0);
    assert(prepare_machine_id(root)!=0);
    assert(unlinkat(dir,"machine-id",0)==0);
    assert(unlinkat(dir,"machine-id.lock",0)==0);close(dir);assert(rmdir(root)==0);
    puts("Machine ID generation, concurrency, persistence and validation passed");return 0;
}
