/* Canonical time is signed 64-bit seconds/nanoseconds. Legacy and time64
 * wire forms are distinct even where libc's time_t is already 64 bits. */
#ifndef PROXY_ARCH_TIME_H
#define PROXY_ARCH_TIME_H
#include <time.h>
struct proxy_timespec {int64_t sec,nsec;};
struct proxy_time32 {int32_t sec,nsec;};
struct proxy_time64 {int64_t sec,nsec;};
static inline long arch_timeout_read(void *wire,int time64,struct proxy_timespec *out,
 long (*copy)(void *,const void *,size_t)){
 if(time64){struct proxy_time64 w;long r=copy(&w,wire,sizeof(w));if(!r)*out=(struct proxy_timespec){w.sec,w.nsec};return r;}
 struct timespec w;long r=copy(&w,wire,sizeof(w));if(!r)*out=(struct proxy_timespec){w.tv_sec,w.tv_nsec};
 return r;
}
static inline long arch_clock_time(int clock,struct proxy_timespec *out){
 struct timespec w;long r=RAW(PROXY_SC_clock_gettime,clock,&w,0,0,0,0);
 if(!r)*out=(struct proxy_timespec){w.tv_sec,w.tv_nsec};
 return r;
}
#endif
