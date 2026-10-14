/* Host-only kernel fixture: regular fake /dev/dri nodes become char devices
 * for the loader's raw fstat. All other operations use the real kernel. */
#include "../proxy.h"
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
long gpu_test_raw(long,long,long,long,long,long,long);
long proxy_raw(long n,long a,long b,long c,long d,long e,long f){
    long r=gpu_test_raw(n,a,b,c,d,e,f);
    if(n==SYS_fstat && !r){char path[64]="/proc/self/fd/",rev[24],text[4096];unsigned k=14,j=0;unsigned long num=a;do{rev[j++]='0'+num%10;num/=10;}while(num);while(j)path[k++]=rev[--j];path[k]=0;
        long len=gpu_test_raw(SYS_readlinkat,AT_FDCWD,(long)path,(long)text,sizeof(text)-1,0,0);
        if(len>0){text[len]=0;const char *name=text;for(long i=0;i<len;i++)if(text[i]=='/')name=text+i+1;
            if(p_equal(name,"renderD129")||p_equal(name,"card2")||p_equal(name,"card3")){struct stat *st=(void *)b;st->st_mode=S_IFCHR|0600;st->st_rdev=makedev(226,p_equal(name,"card2")?2:p_equal(name,"card3")?3:129);}}
    }
    return r;
}
