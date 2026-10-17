#include "buffer_release.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-server-core.h>

/* A stalled SurfaceFlinger must cost bounded storage and no extra threads.
 * Quarantined slots remain occupied until process exit: an error is not proof
 * that an Android reader has stopped. Backpressure occurs BEFORE submission. */
#define RELEASE_CAPACITY 128
enum State { FREE, RESERVED, READY, WATCHING, QUARANTINED };
struct MatonBufferRelease {
  enum State state;
  void* buffer;
  int fence;
  struct wl_event_source* source;
};
static struct {
  pthread_mutex_t mutex;
  struct wl_event_loop* loop;
  struct wl_event_source* wake;
  int fd;
  void (*release)(void*);
  struct MatonBufferRelease slots[RELEASE_CAPACITY];
} pending = {.mutex=PTHREAD_MUTEX_INITIALIZER,.fd=-1};

static void retire(struct MatonBufferRelease* slot) {
  /* Loop thread only, invoked only after a successful fence signal. Leave the
   * slot occupied while calling release (which may destroy Wayland resources). */
  if(slot->source){wl_event_source_remove(slot->source);slot->source=NULL;}
  if(slot->fence>=0){close(slot->fence);slot->fence=-1;}
  pending.release(slot->buffer);
  pthread_mutex_lock(&pending.mutex);
  slot->buffer=NULL;slot->state=FREE;
  pthread_mutex_unlock(&pending.mutex);
}
static int on_fence(int fd, uint32_t mask, void* data) {
  (void)mask;
  struct MatonBufferRelease* slot=data;
  struct pollfd p={.fd=fd,.events=POLLIN};
  int result;
  do result=poll(&p,1,0);while(result<0 && errno==EINTR);
  if(result>0 && (p.revents&POLLIN) && !(p.revents&(POLLERR|POLLHUP|POLLNVAL)))retire(slot);
  else if(result<0 || (result>0 && (p.revents&(POLLERR|POLLHUP|POLLNVAL)))) {
    wl_event_source_remove(slot->source);slot->source=NULL;
    close(slot->fence);slot->fence=-1;
    pthread_mutex_lock(&pending.mutex);slot->state=QUARANTINED;pthread_mutex_unlock(&pending.mutex);
  }
  return 0;
}
static int on_wake(int fd, uint32_t mask, void* data) {
  (void)mask;(void)data;
  uint64_t value;
  while(read(fd,&value,sizeof(value))<0 && errno==EINTR){}
  for(unsigned i=0;i<RELEASE_CAPACITY;i++) {
    struct MatonBufferRelease* slot=&pending.slots[i];
    pthread_mutex_lock(&pending.mutex);
    bool ready=slot->state==READY;
    if(ready)slot->state=WATCHING;
    pthread_mutex_unlock(&pending.mutex);
    if(!ready)continue;
    if(slot->fence==-1){retire(slot);continue;}
    if(slot->fence>=0)slot->source=wl_event_loop_add_fd(pending.loop,slot->fence,WL_EVENT_READABLE,on_fence,slot);
    if(!slot->source) {
      /* epoll allocation/invalid-fd failure cannot free a GPU reader. */
      if(slot->fence>=0)close(slot->fence);
      slot->fence=-1;
      pthread_mutex_lock(&pending.mutex);slot->state=QUARANTINED;pthread_mutex_unlock(&pending.mutex);
    }
  }
  return 0;
}
bool maton_buffer_release_start(struct wl_event_loop* loop, void (*release)(void*)) {
  if(!loop || !release || pending.loop)return false;
  int fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
  if(fd<0)return false;
  struct wl_event_source* source=wl_event_loop_add_fd(loop,fd,WL_EVENT_READABLE,on_wake,NULL);
  if(!source){close(fd);return false;}
  pthread_mutex_lock(&pending.mutex);
  pending.loop=loop;pending.fd=fd;pending.wake=source;pending.release=release;
  pthread_mutex_unlock(&pending.mutex);
  return true;
}
void maton_buffer_release_stop(void) {
  pthread_mutex_lock(&pending.mutex);
  if(pending.wake)wl_event_source_remove(pending.wake);
  pending.wake=NULL;pending.loop=NULL;
  if(pending.fd>=0)close(pending.fd);
  pending.fd=-1;
  for(unsigned i=0;i<RELEASE_CAPACITY;i++) {
    struct MatonBufferRelease* slot=&pending.slots[i];
    if(slot->source){wl_event_source_remove(slot->source);slot->source=NULL;}
    if(slot->state!=FREE) {
      if(slot->fence>=0)close(slot->fence);
      slot->fence=-1;slot->state=QUARANTINED;
    }
  }
  pthread_mutex_unlock(&pending.mutex);
}
struct MatonBufferRelease* maton_buffer_release_reserve(void* buffer) {
  struct MatonBufferRelease* result=NULL;
  pthread_mutex_lock(&pending.mutex);
  if(pending.loop && buffer)for(unsigned i=0;i<RELEASE_CAPACITY;i++) {
    if(pending.slots[i].state==FREE) {
      result=&pending.slots[i];result->state=RESERVED;result->buffer=buffer;result->fence=-1;break;
    }
  }
  pthread_mutex_unlock(&pending.mutex);
  return result;
}
void maton_buffer_release_cancel(struct MatonBufferRelease* slot) {
  if(!slot)return;
  pthread_mutex_lock(&pending.mutex);
  if(slot->state==RESERVED){slot->state=FREE;slot->buffer=NULL;}
  pthread_mutex_unlock(&pending.mutex);
}
void maton_buffer_release_complete(void* opaque, int fence) {
  struct MatonBufferRelease* slot=opaque;
  pthread_mutex_lock(&pending.mutex);
  if(slot->state==RESERVED && pending.loop) {
    slot->fence=fence;slot->state=READY;
    uint64_t one=1;ssize_t result;
    do result=write(pending.fd,&one,sizeof(one));while(result<0 && errno==EINTR);
    /* EAGAIN means a wake is already pending; other errors quarantine safely. */
    if(result<0 && errno!=EAGAIN) {
      if(fence>=0)close(fence);
      slot->fence=-1;slot->state=QUARANTINED;
    }
  }else if(fence>=0)close(fence);
  pthread_mutex_unlock(&pending.mutex);
}
