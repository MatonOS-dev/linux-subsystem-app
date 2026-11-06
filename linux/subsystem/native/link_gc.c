#include <jni.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

JNIEXPORT jint JNICALL
Java_org_matonos_linuxhost_stubgen_PlaceholderCleanup_00024Native_lock(JNIEnv *env, jclass cls, jstring path)
{
    (void)cls;
    const char *store=(*env)->GetStringUTFChars(env,path,NULL);
    if(!store)return -1;
    struct stat directory;
    if(lstat(store,&directory)<0 || !S_ISDIR(directory.st_mode) ||
       directory.st_uid!=geteuid() || (directory.st_mode&0077)) {
        (*env)->ReleaseStringUTFChars(env,path,store);
        (*env)->ThrowNew(env,(*env)->FindClass(env,"java/io/IOException"),"Invalid compat-proxy hardlink store");
        return -1;
    }
    char name[PATH_MAX];
    int n=snprintf(name,sizeof(name),"%s/lock",store);
    (*env)->ReleaseStringUTFChars(env,path,store);
    int fd=-1, error=ENAMETOOLONG;
    if(n>=0 && (size_t)n<sizeof(name)) {
        for(;;) {
            fd=open(name,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
            if(fd<0) { error=errno; break; }
            int rc;
            do { rc=flock(fd,LOCK_EX); } while(rc<0 && errno==EINTR);
            if(rc<0) { error=errno; close(fd);fd=-1;break; }
            struct stat own,named;
            if(fstat(fd,&own)<0 || lstat(name,&named)<0) {
                error=errno;close(fd);fd=-1;break;
            }
            if(own.st_dev==named.st_dev && own.st_ino==named.st_ino)return fd;
            close(fd); // Proxy may have removed an empty store while we waited.
        }
    }
    (*env)->ThrowNew(env,(*env)->FindClass(env,"java/io/IOException"),strerror(error));
    return -1;
}
JNIEXPORT void JNICALL
Java_org_matonos_linuxhost_stubgen_PlaceholderCleanup_00024Native_unlock(JNIEnv *env,jclass cls,jint fd)
{
    (void)env;(void)cls;
    flock(fd,LOCK_UN);
    close(fd);
}
