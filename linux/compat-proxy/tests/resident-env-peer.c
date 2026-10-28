/* Inspect the kernel-exec environment before importing payload state.
 * Host filters require the fixed raw gateway until the resident starts. */
#include <stddef.h>
extern long proxy_raw(long,long,long,long,long,long,long);
/* Match the gateway the inherited host filter allows before state import. */
__asm__(".pushsection .proxy_gateway,\"ax\",@progbits\n"
        ".global proxy_raw\nproxy_raw:\n"
        "mov %rdi,%rax\nmov %rsi,%rdi\nmov %rdx,%rsi\n"
        "mov %rcx,%rdx\nmov %r8,%r10\nmov %r9,%r8\nmov 8(%rsp),%r9\n"
        ".org 0x40,0x90\nsyscall\nret\n.popsection\n");
static int prefix(const char *s,const char *key){while(*key)if(*s++!=*key++)return 0;return 1;}
int main(int argc,char **argv,char **env) {
 int code=81;const char *real=NULL;
 if(argc<5 || !prefix(argv[1],"--resident-state"))goto fail;
 for(unsigned i=0;env[i];i++){
  if(prefix(env[i],"LD_")){
   static const char msg[]="native resident received payload LD_*\n";
   proxy_raw(1,2,(long)msg,sizeof(msg)-1,0,0,0);code=82;goto fail;
  }
  if(prefix(env[i],"MATON_TEST_REAL_RESIDENT="))real=env[i]+sizeof("MATON_TEST_REAL_RESIDENT=")-1;
 }
 if(!real){code=83;goto fail;}
 proxy_raw(59,(long)real,(long)argv,(long)env,0,0,0);code=84;
fail:
 proxy_raw(231,code,0,0,0,0,0);return code;
}
