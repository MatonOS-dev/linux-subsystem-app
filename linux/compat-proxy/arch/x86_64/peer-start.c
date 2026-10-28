/* SPDX-License-Identifier: Apache-2.0
 * No-libc ELF entry for the bionic ELF64 peer. Kernel exec resets handlers,
 * so every syscall before bionic starts uses the inherited fixed gateway.
 *
 * Stock bionic cannot choose a dependency reservation during initial linking.
 * Temporarily occupy the other free holes, letting its normal allocator map
 * the complete initial dependency graph inside the session's exempt region.
 * Release those holes before entering the peer; keep unused native-region
 * holes reserved so subsequent payload mappings cannot accidentally use it.
 */
#include <elf.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#define REGION_SIZE UINT64_C(0x400000000)
#define REGION_KEY "MATON_V3_BIONIC64_BASE="
#define FIXED_BASE UINT64_C(0x600000000000)
#define NOREPLACE 0x100000
#define TEMP_NAME "matonos-bionic64-startup"
extern long peer_raw(long,long,long,long,long,long,long);
extern void _start(void);
extern void peer_enter(void *,void *);
static uint64_t region;
static Elf64_auxv_t *saved_aux;
static uintptr_t down(uintptr_t x){return x&~(uintptr_t)4095;}
static uintptr_t up(uintptr_t x){return (x+4095)&~(uintptr_t)4095;}
static uintptr_t length(const char *s){uintptr_t n=0;while(s[n])n++;return n;}
static int equal(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
__attribute__((noreturn)) static void fail(const char *s){
 const char *prefix="MatonOS x86_64 peer startup: ";
 peer_raw(1,2,(long)prefix,length(prefix),0,0,0);
 peer_raw(1,2,(long)s,length(s),0,0,0);peer_raw(1,2,(long)"\n",1,0,0,0);
 peer_raw(231,111,0,0,0,0,0);__builtin_trap();
}
static void *mapping(uintptr_t size){
 long r=peer_raw(9,0,size,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if((uintptr_t)r>=(uintptr_t)-4095)fail("scratch allocation failed");
 return (void *)r;
}
static char *maps;
static uintptr_t capacity;
static char *read_maps(void){
 if(!maps){capacity=16384;maps=mapping(capacity);}
 long fd=peer_raw(257,-100,(long)"/proc/self/maps",O_RDONLY|O_CLOEXEC,0,0,0);
 if(fd<0)fail("cannot read native mapping layout");
 uintptr_t used=0;
 for(;;){
  if(used==capacity-1){
   if(capacity>UINT64_MAX/2)fail("mapping layout overflow");
   capacity*=2;maps=mapping(capacity);
   /* Keep the earlier scratch mapping occupied until handoff as well. It
    * must not become an unreserved outside hole during native startup. */
   if(peer_raw(8,fd,0,0,0,0,0)<0)fail("cannot rewind mapping layout");
   used=0;
  }
  long n=peer_raw(0,fd,(long)(maps+used),capacity-used-1,0,0,0);
  if(n<0)fail("mapping layout read failed");if(!n)break;used+=(uintptr_t)n;
 }
 peer_raw(3,fd,0,0,0,0,0);maps[used]=0;return maps;
}
static uintptr_t hex(char **p){
 uintptr_t n=0,digits=0;
 for(;;){char c=**p;uintptr_t d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:16;
  if(d==16){if(!digits)fail("invalid mapping address");return n;}
  if(digits++==16)fail("mapping address overflow");n=n*16+d;(*p)++;
 }
}
static void reserve(uintptr_t lo,uintptr_t hi,int temporary){
 if(lo>=hi)return;
 if(peer_raw(9,lo,hi-lo,0,MAP_PRIVATE|MAP_ANONYMOUS|NOREPLACE,-1,0)!=(long)lo)
  fail("native address reservation collided");
 if(temporary && peer_raw(157,0x53564d41,0,lo,hi-lo,(long)TEMP_NAME,0))
  fail("cannot label temporary native reservation");
}
static void hole(uintptr_t lo,uintptr_t hi,int inside){
 if(inside){
  if(lo<region)lo=region;if(hi>region+REGION_SIZE)hi=region+REGION_SIZE;
  reserve(lo,hi,0);
 }else{
  if(lo<region)reserve(lo,hi<region?hi:region,1);
  if(hi>region+REGION_SIZE)reserve(lo>region+REGION_SIZE?lo:region+REGION_SIZE,hi,1);
 }
}
static void occupy_holes(int inside){
 uintptr_t last=65536;
 for(char *p=read_maps();*p;){
  uintptr_t lo=hex(&p);if(*p++!='-')fail("invalid mapping layout");uintptr_t hi=hex(&p);
  if(lo>=UINT64_C(0x800000000000))break;
  if(last<lo)hole(last,lo,inside);last=hi;
  while(*p && *p!='\n')p++;if(*p)p++;
 }
 /* Ordinary x86_64 mappings live below the 47-bit userspace ceiling.
  * The legacy vsyscall mapping above it is not an allocator hole. */
 if(last<0x7ffffffff000)hole(last,0x7ffffffff000,inside);
}
static void release_holes(void){
 for(char *p=read_maps();*p;){
  uintptr_t lo=hex(&p);if(*p++!='-')fail("invalid mapping layout");uintptr_t hi=hex(&p);
  char *line=p;while(*p && *p!='\n')p++;
  if(*p)*p++=0;
  for(char *q=line;*q;q++)if(equal(q,"[anon:" TEMP_NAME "]")){
   if(peer_raw(11,lo,hi-lo,0,0,0,0))fail("cannot release startup reservation");break;
  }
 }
}
static void *symbol(uintptr_t base,const char *name){
 Elf64_Ehdr *eh=(void *)base;Elf64_Phdr *ph=(void *)(base+eh->e_phoff);Elf64_Dyn *dyn=0;
 for(uintptr_t i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_DYNAMIC)dyn=(void *)(base+ph[i].p_vaddr);
 if(!dyn)fail("linker has no dynamic symbols");
 Elf64_Sym *sy=0;char *st=0;uint32_t *hash=0,*gnu=0;
 for(;dyn->d_tag;dyn++){
  if(dyn->d_tag==DT_SYMTAB)sy=(void *)(base+dyn->d_un.d_ptr);
  if(dyn->d_tag==DT_STRTAB)st=(void *)(base+dyn->d_un.d_ptr);
  if(dyn->d_tag==DT_HASH)hash=(void *)(base+dyn->d_un.d_ptr);
  if(dyn->d_tag==DT_GNU_HASH)gnu=(void *)(base+dyn->d_un.d_ptr);
 }
 if(!sy||!st||(!hash&&!gnu))fail("linker symbol table unavailable");
 uintptr_t count=hash?hash[1]:gnu[1];
 if(!hash){uint32_t *bucket=gnu+4+2*gnu[2],*chain=bucket+gnu[0];
  for(uintptr_t i=0;i<gnu[0];i++){uintptr_t n=bucket[i];if(!n)continue;while(!(chain[n-gnu[1]]&1))n++;if(n+1>count)count=n+1;}
 }
 for(uintptr_t i=0;i<count;i++)if(sy[i].st_shndx!=SHN_UNDEF && equal(st+sy[i].st_name,name))return (void *)(base+sy[i].st_value);
 fail("required linker symbol unavailable");
}
static uintptr_t load_linker(const char *path){
 long fd=peer_raw(257,-100,(long)path,O_RDONLY|O_CLOEXEC,0,0,0);
 if(fd<0)fail("cannot open bionic linker");
 Elf64_Ehdr eh;
 if(peer_raw(17,fd,(long)&eh,sizeof(eh),0,0,0)!=sizeof(eh) ||
    eh.e_ident[0]!=127 || eh.e_ident[1]!='E' || eh.e_ident[2]!='L' || eh.e_ident[3]!='F' ||
    eh.e_ident[EI_CLASS]!=ELFCLASS64 || eh.e_machine!=EM_X86_64 || eh.e_type!=ET_DYN ||
    eh.e_phentsize!=sizeof(Elf64_Phdr) || !eh.e_phnum)fail("invalid bionic ELF64 linker");
 uintptr_t bytes=eh.e_phnum*sizeof(Elf64_Phdr);Elf64_Phdr *ph=mapping(up(bytes));
 if(peer_raw(17,fd,(long)ph,bytes,eh.e_phoff,0,0)!=(long)bytes)fail("cannot read bionic program headers");
 uintptr_t top=0;
 for(uintptr_t i=0;i<eh.e_phnum;i++)if(ph[i].p_type==PT_LOAD && ph[i].p_memsz){
  if(ph[i].p_vaddr>REGION_SIZE || ph[i].p_memsz>REGION_SIZE-ph[i].p_vaddr || ph[i].p_filesz>ph[i].p_memsz ||
     (ph[i].p_vaddr&4095)!=(ph[i].p_offset&4095))fail("invalid bionic load segment");
  uintptr_t end=up(ph[i].p_vaddr+ph[i].p_memsz);if(end>top)top=end;
 }
 if(!top || top>=REGION_SIZE || eh.e_entry>=top)fail("invalid bionic linker extent");
 reserve(region,region+top,0);
 for(uintptr_t i=0;i<eh.e_phnum;i++)if(ph[i].p_type==PT_LOAD && ph[i].p_memsz){
  uintptr_t low=down(ph[i].p_vaddr),file_end=ph[i].p_vaddr+ph[i].p_filesz;
  uintptr_t file_bytes=up(file_end)-low;
  uintptr_t prot=((ph[i].p_flags&PF_R)?1:0)|((ph[i].p_flags&PF_W)?2:0)|((ph[i].p_flags&PF_X)?4:0);
  if(file_bytes && peer_raw(9,region+low,file_bytes,prot,MAP_PRIVATE|MAP_FIXED,fd,down(ph[i].p_offset))!=(long)(region+low))fail("bionic segment mapping failed");
  if(ph[i].p_memsz>ph[i].p_filesz){
   if(file_end&4095){
    if(!(prot&PROT_WRITE) && peer_raw(10,region+down(file_end),4096,prot|PROT_WRITE,0,0,0))fail("bionic BSS protection failed");
    for(uintptr_t j=file_end;j<up(file_end);j++)*(char *)(region+j)=0;
    if(!(prot&PROT_WRITE) && peer_raw(10,region+down(file_end),4096,prot,0,0,0))fail("bionic protection restore failed");
   }
   uintptr_t end=up(ph[i].p_vaddr+ph[i].p_memsz),start=up(file_end);
   if(end>start && peer_raw(9,region+start,end-start,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=(long)(region+start))fail("bionic BSS mapping failed");
  }
 }
 peer_raw(3,fd,0,0,0,0,0);
 peer_raw(11,(long)ph,up(bytes),0,0,0,0);
 return region+eh.e_entry;
}
void peer_start(uintptr_t *stack){
 uintptr_t argc=*stack;char **av=(void *)(stack+1),**env=av+argc+1,**end=env;while(*end)end++;
 Elf64_auxv_t *aux=saved_aux?saved_aux:(void *)(end+1),*atbase=0;
 saved_aux=aux;
 for(Elf64_auxv_t *a=aux;a->a_type;a++)if(a->a_type==AT_BASE)atbase=a;
 if(!atbase)fail("missing AT_BASE");
 if(!atbase->a_un.a_val){
  for(char **e=env;*e;e++){uintptr_t k=0;const char *key=REGION_KEY;while(key[k] && key[k]==(*e)[k])k++;
   if(!key[k]){uintptr_t n=0;const char *p=*e+k;if(!*p)fail("empty native base");
    for(;*p;p++){if(*p<'0'||*p>'9'||n>(UINT64_MAX-(uintptr_t)(*p-'0'))/10)fail("invalid native base");n=n*10+*p-'0';}region=n;}
  }
  if(!region){uint32_t random;if(peer_raw(318,(long)&random,4,0,0,0,0)!=4)fail("native address randomization failed");region=(UINT64_C(0x20)+(random%0x1000))*UINT64_C(0x100000000);}
  if((region&UINT64_C(0xffffffff)) || region<UINT64_C(0x2000000000) || region>UINT64_C(0x101f00000000))fail("native base outside session address range");
#ifdef PEER_TEST_LINKER
  const char *linker=PEER_TEST_LINKER;
#else
  const char *linker="/system/bin/linker64";
#endif
  uintptr_t entry=load_linker(linker);
  /* ET_EXEC pins the fixed gateway for the kernel. Bionic's ELF validation
   * wants ET_DYN, with zero bias for our already-mapped bootstrap image. */
  Elf64_Ehdr *self=(void *)FIXED_BASE;
  if(peer_raw(10,(long)self,4096,3,0,0,0))fail("bootstrap ELF header protection failed");
  self->e_type=ET_DYN;
  if(peer_raw(10,(long)self,4096,1,0,0,0))fail("bootstrap ELF header restore failed");
  occupy_holes(0);
  /* Disable fallback syscall sites in the random kernel vDSO mapping. */
  for(Elf64_auxv_t *a=aux;a->a_type;a++)if(a->a_type==AT_SYSINFO_EHDR)a->a_un.a_val=0;
  atbase->a_un.a_val=region;
  peer_enter((void *)entry,stack);
 }
 /* Stock bionic has relocated us and initialized libc/TLS/DT_NEEDED peers. */
 region=atbase->a_un.a_val;
 /* Free the temporary outside holes first: a large dependency graph can
  * grow /proc/self/maps past the scratch capacity. No payload exists yet. */
 release_holes();
 occupy_holes(1);
 void *(*lookup)(void *,const char *,const void *)=symbol(region,"__loader_dlsym");
 uint64_t *base=lookup((void *)-1,"proxy_bionic64_base",_start);
 int (*run)(int,char **,char **)=lookup((void *)-1,"main",_start);
 if(!base||!run)fail("native peer entry unavailable");*base=region;
 int *active=lookup((void *)-1,"proxy_bionic64_active",_start);
 if(!active)fail("native peer marker unavailable");*active=1;
 int result=run((int)argc,av,env);
 peer_raw(231,result,0,0,0,0,0);__builtin_trap();
}
