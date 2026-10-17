#include "buffer_release.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-server-core.h>

static unsigned released;
static pthread_t loop_thread;
static void release(void* buffer) {
  assert(buffer);
  assert(pthread_equal(pthread_self(),loop_thread));
  released++;
}
static void dispatch(struct wl_event_loop* loop) {
  for(int i=0;i<3;i++)assert(wl_event_loop_dispatch(loop,0)==0);
}
struct Complete { struct MatonBufferRelease* slot; int fence; };
static void* complete(void* p) {
  struct Complete* c=p;
  maton_buffer_release_complete(c->slot,c->fence);
  return NULL;
}
static int fail_add;
struct wl_event_source* __real_wl_event_loop_add_fd(struct wl_event_loop*,int,uint32_t,wl_event_loop_fd_func_t,void*);
struct wl_event_source* __wrap_wl_event_loop_add_fd(struct wl_event_loop* loop,int fd,uint32_t mask,wl_event_loop_fd_func_t callback,void* data) {
  if(fail_add)return NULL;
  return __real_wl_event_loop_add_fd(loop,fd,mask,callback,data);
}
int main(void) {
  int buffer;
  loop_thread=pthread_self();
  struct wl_event_loop* loop=wl_event_loop_create();assert(loop);
  assert(!maton_buffer_release_reserve(&buffer));
  assert(!maton_buffer_release_start(NULL,release));
  fail_add=1;assert(!maton_buffer_release_start(loop,release));fail_add=0;
  assert(maton_buffer_release_start(loop,release));
  assert(!maton_buffer_release_start(loop,release));
  struct MatonBufferRelease* slot=maton_buffer_release_reserve(&buffer);assert(slot);
  int fence=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);assert(fence>=0);
  int signal=dup(fence);assert(signal>=0);
  struct Complete c={slot,fence};pthread_t thread;
  assert(!pthread_create(&thread,NULL,complete,&c));assert(!pthread_join(thread,NULL));
  dispatch(loop);assert(released==0); /* Never release an unsignaled fence. */
  uint64_t one=1;assert(write(signal,&one,sizeof(one))==sizeof(one));
  dispatch(loop);assert(released==1);close(signal);
  slot=maton_buffer_release_reserve(&buffer);assert(slot);
  maton_buffer_release_cancel(slot);
  struct MatonBufferRelease* slots[128];
  for(unsigned i=0;i<128;i++){slots[i]=maton_buffer_release_reserve(&buffer);assert(slots[i]);}
  assert(!maton_buffer_release_reserve(&buffer));
  for(unsigned i=0;i<128;i++)maton_buffer_release_complete(slots[i],-1);
  assert(!maton_buffer_release_reserve(&buffer)); /* Remain occupied until loop dispatch. */
  dispatch(loop);assert(released==129);
  slot=maton_buffer_release_reserve(&buffer);assert(slot);
  fence=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);assert(fence>=0);
  maton_buffer_release_complete(slot,fence);fail_add=1;dispatch(loop);fail_add=0;
  assert(released==129); /* epoll/OOM failure quarantines; no unsafe unlock. */
  int pipes[2];assert(!pipe(pipes));
  slot=maton_buffer_release_reserve(&buffer);assert(slot);
  maton_buffer_release_complete(slot,pipes[0]);dispatch(loop);close(pipes[1]);
  dispatch(loop);assert(released==129); /* HUP is not a signaled sync_file. */
  /* Two permanent quarantines consume capacity; all other reservations return. */
  for(unsigned i=0;i<126;i++){slots[i]=maton_buffer_release_reserve(&buffer);assert(slots[i]);}
  assert(!maton_buffer_release_reserve(&buffer));
  for(unsigned i=0;i<125;i++)maton_buffer_release_cancel(slots[i]);
  slot=slots[125]; /* callback outstanding across shutdown */
  maton_buffer_release_stop();
  fence=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);assert(fence>=0);
  maton_buffer_release_complete(slot,fence);assert(released==129);
  assert(!maton_buffer_release_reserve(&buffer));
  assert(maton_buffer_release_start(loop,release));
  for(unsigned i=0;i<125;i++){slots[i]=maton_buffer_release_reserve(&buffer);assert(slots[i]);}
  assert(!maton_buffer_release_reserve(&buffer));
  for(unsigned i=0;i<125;i++)maton_buffer_release_cancel(slots[i]);
  maton_buffer_release_stop();wl_event_loop_destroy(loop);
  puts("PASS release: unsignaled/signaled fences, loop-thread dispatch, 128-slot backpressure, OOM/HUP quarantine, late callback and restart");
  return 0;
}
