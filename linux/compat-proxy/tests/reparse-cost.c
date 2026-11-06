/* Count the link layer directly, excluding independent VFS resolution. */
#include "../proxy.c"
#define CHECK(x) do{if(!(x))return __LINE__;}while(0)
int main(void){
 const char *path="/proc/version";
 proxy_arg_t a[6]={AT_FDCWD,(proxy_arg_t)path,O_RDONLY,0,0,0};
 proxy_raw_count_enable();
 uint64_t before=proxy_raw_count_now();
 long fd=links_path_call(PROXY_SC_openat,a,path);CHECK(fd>=0);
 CHECK(proxy_raw_count_now()-before==1);
 struct stat st;a[2]=(proxy_arg_t)&st;a[3]=0;
 before=proxy_raw_count_now();CHECK(!links_path_call(PROXY_SC_newfstatat,a,path));
 CHECK(proxy_raw_count_now()-before==1);
 before=proxy_raw_count_now();CHECK(!RAW(PROXY_SC_fstat,fd,&st,0,0,0,0));links_payload_fd_stat(fd,&st);
 CHECK(proxy_raw_count_now()-before==1);
 struct statx sx;a[2]=0;a[3]=STATX_BASIC_STATS;a[4]=(proxy_arg_t)&sx;
 before=proxy_raw_count_now();CHECK(!links_path_call(PROXY_SC_statx,a,path));
 CHECK(proxy_raw_count_now()-before==1);
 RAW(PROXY_SC_close,fd,0,0,0,0,0);
 a[2]=O_PATH;a[3]=0;a[4]=0;
 before=proxy_raw_count_now();fd=links_path_call(PROXY_SC_openat,a,path);CHECK(fd>=0);
 CHECK(proxy_raw_count_now()-before==2);
 RAW(PROXY_SC_close,fd,0,0,0,0,0);
 const char msg[]="PASS link-layer raw syscall counts: open/stat/statx/fstat=1, O_PATH regular open=2; no locks\n";
 RAW(PROXY_SC_write,1,msg,sizeof(msg)-1,0,0,0);return 0;
}
void _start(void){RAW(PROXY_SC_exit_group,main(),0,0,0,0,0);}
