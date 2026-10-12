#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
int main(void){errno=0;if(syscall(SYS_getppid)!=-1 || errno!=EACCES)return 1;puts("filter-ok");return 0;}
