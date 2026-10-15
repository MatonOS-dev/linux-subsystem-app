#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct ShmName {
  char* name;
  int fd;
  struct ShmName* next;
};
static pthread_mutex_t names_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ShmName* names;

int maton_shm_open(const char* name, int flags, mode_t mode) {
  (void)mode;
  if (!name || name[0] != '/') { errno = EINVAL; return -1; }
  pthread_mutex_lock(&names_lock);
  for (struct ShmName* it=names; it; it=it->next) {
    if (strcmp(it->name,name)==0) {
      if ((flags & (O_CREAT|O_EXCL)) == (O_CREAT|O_EXCL)) { pthread_mutex_unlock(&names_lock); errno=EEXIST; return -1; }
      char path[64]; snprintf(path,sizeof(path),"/proc/self/fd/%d",it->fd);
      int fd=open(path,(flags&~(O_CREAT|O_EXCL|O_CLOEXEC))|O_CLOEXEC);
      pthread_mutex_unlock(&names_lock);
      return fd;
    }
  }
  if (!(flags & O_CREAT)) { pthread_mutex_unlock(&names_lock); errno=ENOENT; return -1; }
  const char* label=name[1]?name+1:"maton-shm";
  int fd=memfd_create(label,MFD_CLOEXEC);
  if(fd<0){pthread_mutex_unlock(&names_lock);return -1;}
  mode_t mask=umask(0);umask(mask);
  if(fchmod(fd,mode&~mask)!=0){int err=errno;close(fd);pthread_mutex_unlock(&names_lock);errno=err;return -1;}
  struct ShmName* item=calloc(1,sizeof(*item));
  if(!item){close(fd);pthread_mutex_unlock(&names_lock);errno=ENOMEM;return -1;}
  item->name=strdup(name);
  int registry_fd=fcntl(fd,F_DUPFD_CLOEXEC,0);
  item->fd=registry_fd;
  if(!item->name||registry_fd<0){int err=errno;free(item->name);free(item);if(registry_fd>=0)close(registry_fd);close(fd);pthread_mutex_unlock(&names_lock);errno=err;return -1;}
  item->next=names;names=item;
  pthread_mutex_unlock(&names_lock);
  return fd;
}

int maton_shm_unlink(const char* name) {
  if(!name){errno=EINVAL;return -1;}
  pthread_mutex_lock(&names_lock);
  struct ShmName** at=&names;
  while(*at&&strcmp((*at)->name,name)!=0)at=&(*at)->next;
  if(!*at){pthread_mutex_unlock(&names_lock);errno=ENOENT;return -1;}
  struct ShmName* item=*at;*at=item->next;
  pthread_mutex_unlock(&names_lock);
  close(item->fd);free(item->name);free(item);return 0;
}
