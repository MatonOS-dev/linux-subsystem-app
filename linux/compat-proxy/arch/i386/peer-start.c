/* SPDX-License-Identifier: Apache-2.0
 * No-libc ELF entry for the bionic ELF32 peer. Kernel exec resets handlers,
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
#define REGION_SIZE UINT32_C(0x10000000)
#define REGION_KEY "MATON_V3_BIONIC32_BASE="
#define FIXED_BASE UINT32_C(0x70000000)
#define NOREPLACE 0x100000
#define TEMP_NAME "matonos-bionic32-startup"
extern long peer_raw(long,long,long,long,long,long,long);
extern void _start(void);
extern void peer_enter(void *,void *);
static uint32_t region;
static unsigned down(unsigned x){return x&~4095u;}
static unsigned up(unsigned x){return (x+4095)&~4095u;}
static unsigned length(const char *s){unsigned n=0;while(s[n])n++;return n;}
static int equal(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
__attribute__((noreturn)) static void fail(const char *s){
 const char *prefix="MatonOS i386 peer startup: ";
 peer_raw(4,2,(long)prefix,length(prefix),0,0,0);
 peer_raw(4,2,(long)s,length(s),0,0,0);peer_raw(4,2,(long)"\n",1,0,0,0);
 peer_raw(252,111,0,0,0,0,0);__builtin_trap();
}
static void *mapping(unsigned size){
 long r=peer_raw(192,0,size,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if((unsigned long)r>=(unsigned long)-4095)fail("scratch allocation failed");
 return (void *)r;
}
static char *maps;
static unsigned capacity;
static char *read_maps(void){
 if(!maps){capacity=16384;maps=mapping(capacity);}
 long fd=peer_raw(295,-100,(long)"/proc/self/maps",O_RDONLY|O_CLOEXEC,0,0,0);
 if(fd<0)fail("cannot read native mapping layout");
 unsigned used=0;
 for(;;){
  if(used==capacity-1){
   if(capacity>UINT32_MAX/2)fail("mapping layout overflow");
   capacity*=2;maps=mapping(capacity);
   /* Keep the earlier scratch mapping occupied until handoff as well. It
    * must not become an unreserved outside hole during native startup. */
   if(peer_raw(19,fd,0,0,0,0,0)<0)fail("cannot rewind mapping layout");
   used=0;
  }
  long n=peer_raw(3,fd,(long)(maps+used),capacity-used-1,0,0,0);
  if(n<0)fail("mapping layout read failed");if(!n)break;used+=(unsigned)n;
 }
 peer_raw(6,fd,0,0,0,0,0);maps[used]=0;return maps;
}
static unsigned hex(char **p){
 unsigned n=0,digits=0;
 for(;;){char c=**p;unsigned d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:16;
  if(d==16){if(!digits)fail("invalid mapping address");return n;}
  if(digits++==8)fail("mapping address overflow");n=n*16+d;(*p)++;
 }
}
static void reserve(unsigned lo,unsigned hi,int temporary){
 if(lo>=hi)return;
 if(peer_raw(192,lo,hi-lo,0,MAP_PRIVATE|MAP_ANONYMOUS|NOREPLACE,-1,0)!=(long)lo)
  fail("native address reservation collided");
 if(temporary && peer_raw(172,0x53564d41,0,lo,hi-lo,(long)TEMP_NAME,0))
  fail("cannot label temporary native reservation");
}
static void hole(unsigned lo,unsigned hi,int inside){
 if(inside){
  if(lo<region)lo=region;if(hi>region+REGION_SIZE)hi=region+REGION_SIZE;
  reserve(lo,hi,0);
 }else{
  if(lo<region)reserve(lo,hi<region?hi:region,1);
  if(hi>region+REGION_SIZE)reserve(lo>region+REGION_SIZE?lo:region+REGION_SIZE,hi,1);
 }
}
static void occupy_holes(int inside){
 unsigned last=65536;
 for(char *p=read_maps();*p;){
  unsigned lo=hex(&p);if(*p++!='-')fail("invalid mapping layout");unsigned hi=hex(&p);
  if(last<lo)hole(last,lo,inside);last=hi;
  while(*p && *p!='\n')p++;if(*p)p++;
 }
 /* Linux's x86 compat TASK_SIZE is 0xffffe000 on a 64-bit host. The
  * inaccessible final two pages are not candidate library mappings. */
 if(last<0xffffe000)hole(last,0xffffe000,inside);
}
static void release_holes(void){
 for(char *p=read_maps();*p;){
  unsigned lo=hex(&p);if(*p++!='-')fail("invalid mapping layout");unsigned hi=hex(&p);
  char *line=p;while(*p && *p!='\n')p++;
  if(*p)*p++=0;
  for(char *q=line;*q;q++)if(equal(q,"[anon:" TEMP_NAME "]")){
   if(peer_raw(91,lo,hi-lo,0,0,0,0))fail("cannot release startup reservation");break;
  }
 }
}
static void *symbol(unsigned base,const char *name){
 Elf32_Ehdr *eh=(void *)base;Elf32_Phdr *ph=(void *)(base+eh->e_phoff);Elf32_Dyn *dyn=0;
 for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_DYNAMIC)dyn=(void *)(base+ph[i].p_vaddr);
 if(!dyn)fail("linker has no dynamic symbols");
 Elf32_Sym *sy=0;char *st=0;unsigned *hash=0,*gnu=0;
 for(;dyn->d_tag;dyn++){
  if(dyn->d_tag==DT_SYMTAB)sy=(void *)(base+dyn->d_un.d_ptr);
  if(dyn->d_tag==DT_STRTAB)st=(void *)(base+dyn->d_un.d_ptr);
  if(dyn->d_tag==DT_HASH)hash=(void *)(base+dyn->d_un.d_ptr);
  if(dyn->d_tag==DT_GNU_HASH)gnu=(void *)(base+dyn->d_un.d_ptr);
 }
 if(!sy||!st||(!hash&&!gnu))fail("linker symbol table unavailable");
 unsigned count=hash?hash[1]:gnu[1];
 if(!hash){unsigned *bucket=gnu+4+gnu[2],*chain=bucket+gnu[0];
  for(unsigned i=0;i<gnu[0];i++){unsigned n=bucket[i];if(!n)continue;while(!(chain[n-gnu[1]]&1))n++;if(n+1>count)count=n+1;}
 }
 for(unsigned i=0;i<count;i++)if(sy[i].st_shndx!=SHN_UNDEF && equal(st+sy[i].st_name,name))return (void *)(base+sy[i].st_value);
 fail("required linker symbol unavailable");
}
static unsigned load_linker(const char *path){
 long fd=peer_raw(295,-100,(long)path,O_RDONLY|O_CLOEXEC,0,0,0);
 if(fd<0)fail("cannot open bionic linker");
 Elf32_Ehdr eh;
 if(peer_raw(180,fd,(long)&eh,sizeof(eh),0,0,0)!=sizeof(eh) ||
    eh.e_ident[0]!=127 || eh.e_ident[1]!='E' || eh.e_ident[2]!='L' || eh.e_ident[3]!='F' ||
    eh.e_ident[EI_CLASS]!=ELFCLASS32 || eh.e_machine!=EM_386 || eh.e_type!=ET_DYN ||
    eh.e_phentsize!=sizeof(Elf32_Phdr) || !eh.e_phnum)fail("invalid bionic ELF32 linker");
 unsigned bytes=eh.e_phnum*sizeof(Elf32_Phdr);Elf32_Phdr *ph=mapping(up(bytes));
 if(peer_raw(180,fd,(long)ph,bytes,eh.e_phoff,0,0)!=(long)bytes)fail("cannot read bionic program headers");
 unsigned top=0;
 for(unsigned i=0;i<eh.e_phnum;i++)if(ph[i].p_type==PT_LOAD && ph[i].p_memsz){
  if(ph[i].p_vaddr>REGION_SIZE || ph[i].p_memsz>REGION_SIZE-ph[i].p_vaddr || ph[i].p_filesz>ph[i].p_memsz ||
     (ph[i].p_vaddr&4095)!=(ph[i].p_offset&4095))fail("invalid bionic load segment");
  unsigned end=up(ph[i].p_vaddr+ph[i].p_memsz);if(end>top)top=end;
 }
 if(!top || top>=REGION_SIZE || eh.e_entry>=top)fail("invalid bionic linker extent");
 reserve(region,region+top,0);
 for(unsigned i=0;i<eh.e_phnum;i++)if(ph[i].p_type==PT_LOAD && ph[i].p_memsz){
  unsigned low=down(ph[i].p_vaddr),file_end=ph[i].p_vaddr+ph[i].p_filesz;
  unsigned file_bytes=up(file_end)-low;
  unsigned prot=((ph[i].p_flags&PF_R)?1:0)|((ph[i].p_flags&PF_W)?2:0)|((ph[i].p_flags&PF_X)?4:0);
  if(file_bytes && peer_raw(192,region+low,file_bytes,prot,MAP_PRIVATE|MAP_FIXED,fd,down(ph[i].p_offset)/4096)!=(long)(region+low))fail("bionic segment mapping failed");
  if(ph[i].p_memsz>ph[i].p_filesz){
   if(file_end&4095){
    if(!(prot&PROT_WRITE) && peer_raw(125,region+down(file_end),4096,prot|PROT_WRITE,0,0,0))fail("bionic BSS protection failed");
    for(unsigned j=file_end;j<up(file_end);j++)*(char *)(region+j)=0;
    if(!(prot&PROT_WRITE) && peer_raw(125,region+down(file_end),4096,prot,0,0,0))fail("bionic protection restore failed");
   }
   unsigned end=up(ph[i].p_vaddr+ph[i].p_memsz),start=up(file_end);
   if(end>start && peer_raw(192,region+start,end-start,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=(long)(region+start))fail("bionic BSS mapping failed");
  }
 }
 peer_raw(6,fd,0,0,0,0,0);
 peer_raw(91,(long)ph,up(bytes),0,0,0,0);
 return region+eh.e_entry;
}
void peer_start(unsigned *stack){
 unsigned argc=*stack;char **av=(void *)(stack+1),**env=av+argc+1,**end=env;while(*end)end++;
 Elf32_auxv_t *aux=(void *)(end+1),*atbase=0;
 for(Elf32_auxv_t *a=aux;a->a_type;a++)if(a->a_type==AT_BASE)atbase=a;
 if(!atbase)fail("missing AT_BASE");
 if(!atbase->a_un.a_val){
  for(char **e=env;*e;e++){unsigned k=0;const char *key=REGION_KEY;while(key[k] && key[k]==(*e)[k])k++;
   if(!key[k]){unsigned n=0;const char *p=*e+k;if(!*p)fail("empty native base");
    for(;*p;p++){if(*p<'0'||*p>'9'||n>(UINT32_MAX-(unsigned)(*p-'0'))/10)fail("invalid native base");n=n*10+*p-'0';}region=n;}
  }
  if(!region){uint32_t random;if(peer_raw(355,(long)&random,4,0,0,0,0)!=4)fail("native address randomization failed");region=0x10000000+(random%0x40000)*4096;}
  if((region&4095) || region<0x10000000 || region>0x50000000)fail("native base outside low address space");
#ifdef PEER_TEST_LINKER
  const char *linker=PEER_TEST_LINKER;
#else
  const char *linker="/system/bin/linker";
#endif
  unsigned entry=load_linker(linker);
  /* ET_EXEC pins the fixed gateway for the kernel. Bionic's ELF validation
   * wants ET_DYN, with zero bias for our already-mapped bootstrap image. */
  Elf32_Ehdr *self=(void *)FIXED_BASE;
  if(peer_raw(125,(long)self,4096,3,0,0,0))fail("bootstrap ELF header protection failed");
  self->e_type=ET_DYN;
  if(peer_raw(125,(long)self,4096,1,0,0,0))fail("bootstrap ELF header restore failed");
  occupy_holes(0);
  /* Original AT_SYSINFO points into a random vDSO mapping. Its syscall IP
   * is outside the inherited native region. Use our fixed exempt register
   * gateway and disable vDSO fallback syscall sites for the native runtime. */
  for(Elf32_auxv_t *a=aux;a->a_type;a++) {
   if(a->a_type==AT_SYSINFO)a->a_un.a_val=0x70300100;
   if(a->a_type==AT_SYSINFO_EHDR)a->a_un.a_val=0;
  }
  atbase->a_un.a_val=region;
  peer_enter((void *)entry,stack);
 }
 /* Stock bionic has relocated us and initialized libc/TLS/DT_NEEDED peers. */
 region=atbase->a_un.a_val;
 occupy_holes(1);
 release_holes();
 void *(*lookup)(void *,const char *,const void *)=symbol(region,"__loader_dlsym");
 uint32_t *base=lookup((void *)-1,"proxy_bionic32_base",_start);
 int (*run)(int,char **,char **)=lookup((void *)-1,"main",_start);
 if(!base||!run)fail("native peer entry unavailable");*base=region;
 int result=run((int)argc,av,env);
 peer_raw(252,result,0,0,0,0,0);__builtin_trap();
}
