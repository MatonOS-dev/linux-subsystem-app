/* Shared by the launcher and its host-side persistence test. */
#ifndef MATON_MACHINE_ID_H
#define MATON_MACHINE_ID_H
#include <sys/file.h>
#include <sys/random.h>

#define MATON_MACHINE_ID "/data/matonos/linux/machine-id"
static int prepare_machine_id(const char* root) {
    int dir=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0)return -1;
    int lock=openat(dir,"machine-id.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    struct stat st;
    int rc=-1, fd=-1;
    if(lock<0 || fstat(lock,&st) || !S_ISREG(st.st_mode) || st.st_uid!=getuid() ||
            (st.st_mode&077) || flock(lock,LOCK_EX))goto done;
    fd=openat(dir,"machine-id",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd>=0) {
        char text[34]; ssize_t n=read(fd,text,sizeof(text));
        if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=getuid() ||
                (n!=32 && n!=33) || (n==33 && text[32]!='\n'))goto done;
        int nonzero=0;
        for(int i=0;i<32;i++) {
            if(!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f')))goto done;
            nonzero |= text[i]!='0';
        }
        if(nonzero)rc=0;
        goto done;
    }
    if(errno!=ENOENT)goto done;
    unsigned char random[16]; size_t used=0;
    while(used<sizeof(random)) {
        ssize_t n=getrandom(random+used,sizeof(random)-used,0);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)goto done;
        used+=(size_t)n;
    }
    char text[33];static const char hex[]="0123456789abcdef";
    for(int i=0;i<16;i++){text[2*i]=hex[random[i]>>4];text[2*i+1]=hex[random[i]&15];}
    text[32]='\n';
    /* The lock serializes generation; rename publishes only a complete ID.
     * A crash before rename leaves a disposable temporary, never a partial ID. */
    if(unlinkat(dir,"machine-id.tmp",0) && errno!=ENOENT)goto done;
    fd=openat(dir,"machine-id.tmp",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=getuid())goto done;
    used=0;
    while(used<sizeof(text)) {
        ssize_t n=write(fd,text+used,sizeof(text)-used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)goto done;
        used+=(size_t)n;
    }
    if(fchmod(fd,0444) || fsync(fd) || renameat(dir,"machine-id.tmp",dir,"machine-id") || fsync(dir))goto done;
    rc=0;
done:
    if(fd>=0)close(fd);
    if(lock>=0)close(lock);
    close(dir);return rc;
}
#endif
