/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void){
 struct rlimit limit;assert(!getrlimit(RLIMIT_NOFILE,&limit));assert(limit.rlim_cur>64);
 int private_fd=(int)limit.rlim_cur-18+1;assert(fcntl(private_fd,F_GETFD)>=0);
 assert(!syscall(SYS_close_range,3,UINT_MAX,0));assert(fcntl(private_fd,F_GETFD)>=0);
 struct rlimit lowered=limit;lowered.rlim_cur=64;assert(!setrlimit(RLIMIT_NOFILE,&lowered));
 struct rlimit current;assert(!getrlimit(RLIMIT_NOFILE,&current));assert(current.rlim_cur==64);
 assert(!syscall(SYS_close_range,3,UINT_MAX,0));int fd=open("/dev/null",O_RDONLY);assert(fd>=0);close(fd);
 static const char done[]="PASS private fd pool survives close_range and setrlimit lowering\n";
 assert(write(1,done,sizeof(done)-1)==sizeof(done)-1);
 return 0;
}
