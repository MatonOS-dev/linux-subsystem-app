#!/usr/bin/env python3
"""Required real OSTree -> aligned ZIP -> resident proxy host acceptance."""
import os,pathlib,subprocess,sys,shutil,hashlib,json,zipfile
base=pathlib.Path(__file__).resolve().parents[1]; root=base/'build/pack-host'
if root.exists():shutil.rmtree(root)
root.mkdir();src=root/'src';src.mkdir();(src/'nested').mkdir();(src/'lib').mkdir()
def run(*args,**kw):return subprocess.run(list(map(str,args)),check=True,**kw)
def output(*args,**kw):return subprocess.check_output(list(map(str,args)),**kw)
run('cc','-m32','-nostdlib','-static','-no-pie',base/'tests/pack-peer.S','-o',src/'peer')
(src/'lib/x86_64-linux-gnu/GL/default').mkdir(parents=True)
(src/'lib/x86_64-linux-gnu/GL/default/extension-marker').write_text('GL extension directory entry\n')
(src/'etc').mkdir();(src/'etc/e2scrub.conf').write_text('packed regular file bind identity\n')
(src/'bin').mkdir()
(root/'true.c').write_text('int main(void){return 0;}\n')
run('cc','-static','-O2','-Wall','-Wextra','-Werror',root/'true.c','-o',src/'bin/true')
(root/'bind-entry-probe.c').write_text(r'''#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void){DIR*d=opendir("/usr/lib/x86_64-linux-gnu/GL/vulkan/icd.d");if(!d){perror("opendir bound parent");return 1;}int found=0,phantom=0;struct dirent*e;while((e=readdir(d))){if(!strcmp(e->d_name,"maton_icd.json")){if(e->d_type!=DT_REG){fprintf(stderr,"wrong d_type: %u\n",e->d_type);return 2;}found++;}if(!strcmp(e->d_name,"never-created"))phantom++;}closedir(d);if(found!=1){fprintf(stderr,"bound entry count: %d\n",found);return 3;}if(phantom)return 6;int fd=open("/usr/lib/x86_64-linux-gnu/GL/vulkan/icd.d/maton_icd.json",O_RDONLY);if(fd<0)return 4;char b[64]={0};ssize_t n=read(fd,b,sizeof(b)-1);close(fd);if(n<0||strcmp(b,"newly bound Vulkan ICD entry\n"))return 5;puts("bwrap packed-parent bind entry readdir/d_type/open PASS");return 0;}
''')
run('cc','-static','-O2','-Wall','-Wextra','-Werror',root/'bind-entry-probe.c','-o',src/'bin/probe-new-bind')
(root/'file-bind-launcher.c').write_text(r'''#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int main(int argc,char **argv){if(argc!=2)return 1;int usr=open("/usr",O_PATH|O_DIRECTORY);int file=open("/usr/etc/e2scrub.conf",O_PATH);if(usr<0||file<0)return 2;char u[24],f[24];snprintf(u,sizeof(u),"%d",usr);snprintf(f,sizeof(f),"%d",file);char *args[]={argv[1],"--ro-bind","/","/","--ro-bind-fd",u,"/usr","--ro-bind-fd",f,"/usr/etc/e2scrub.conf","--proc","/proc","--dev","/dev","/usr/bin/true",NULL};execv(argv[1],args);return 3;}
''')
run('cc','-static','-O2','-Wall','-Wextra','-Werror',root/'file-bind-launcher.c','-o',root/'file-bind-launcher')
(root/'dir-mount.c').write_text(r'''#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void){struct stat st;if(stat("/usr/lib/x86_64-linux-gnu/GL",&st)||!S_ISDIR(st.st_mode))return 1;DIR*d=opendir("/usr/lib/x86_64-linux-gnu/GL");if(!d)return 2;int found=0;struct dirent*e;while((e=readdir(d)))if(!strcmp(e->d_name,"default"))found=1;closedir(d);if(!found)return 3;int fd=open("/usr/lib/x86_64-linux-gnu/GL/default/extension-marker",O_RDONLY);if(fd<0)return 4;char b[64]={0};if(read(fd,b,sizeof(b)-1)<0)return 5;close(fd);if(strcmp(b,"GL extension directory entry\n"))return 6;puts("packed directory lookup through bind source PASS");return 0;}
''')
run('cc','-static','-O2','-Wall','-Wextra','-Werror','-include','string.h',root/'dir-mount.c','-o',src/'probe-dir-mount')
run('cc','-static','-O2','-Wall','-Wextra','-Werror',base/'tests/pack-vfd.c','-o',src/'probe')
(root/'root-dir.c').write_text(r'''#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc,char**argv){const char*parent_path=argc>1?argv[1]:"/";const char*name=argc>2?argv[2]:"packed";const char*entry=argc>3?argv[3]:"data";char pathbuf[128];snprintf(pathbuf,sizeof(pathbuf),"%s/%s",!strcmp(parent_path,"/")?"":parent_path,name);int parent=open(parent_path,O_PATH|O_DIRECTORY);if(parent<0)return 1;struct stat st;if(fstatat(parent,name,&st,AT_SYMLINK_NOFOLLOW)||!S_ISDIR(st.st_mode)||lstat(pathbuf,&st)||!S_ISDIR(st.st_mode))return 2;struct statx sx;if(syscall(SYS_statx,parent,name,AT_SYMLINK_NOFOLLOW,STATX_TYPE,&sx)||!(sx.stx_mode&S_IFDIR))return 3;int path=openat(parent,name,O_PATH|O_NOFOLLOW|O_DIRECTORY);if(path<0||fstat(path,&st)||!S_ISDIR(st.st_mode))return 4;close(path);int dir=openat(parent,name,O_RDONLY|O_DIRECTORY);if(dir<0)return 5;char buf[4096];int found=0;for(;;){int n=syscall(SYS_getdents64,dir,buf,sizeof(buf));if(n<0)return 6;if(!n)break;for(int p=0;p<n;){struct linux_dirent64{unsigned long long ino;long long off;unsigned short reclen;unsigned char type;char name[];}*e=(void*)(buf+p);if(!strcmp(e->name,entry))found=1;p+=e->reclen;}}if(!found)return 7;if(access(pathbuf,R_OK|X_OK))return 8;close(dir);close(parent);puts("packed root stat/lstat/statx/openat O_PATH/getdents/access PASS");return 0;}
''')
run('cc','-static','-O2','-Wall','-Wextra','-Werror','-include','sys/syscall.h',root/'root-dir.c','-o',src/'probe-root-dir')
(root/'root-identity.c').write_text(r'''#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
int main(int argc,char**argv){if(argc!=3)return 1;int source=open(argv[1],O_PATH|O_DIRECTORY);if(source<0)return 2;struct stat a;if(fstat(source,&a))return 3;if(mount(argv[1],argv[2],NULL,MS_BIND,NULL))return 4;int dest=openat(AT_FDCWD,argv[2],O_PATH|O_DIRECTORY);if(dest<0)return 5;struct stat b,c;if(fstat(dest,&b)||stat(argv[2],&c))return 6;if(a.st_dev!=b.st_dev||a.st_ino!=b.st_ino||a.st_dev!=c.st_dev||a.st_ino!=c.st_ino)return 7;int d=open(argv[2],O_RDONLY|O_DIRECTORY);if(d<0)return 8;DIR*dp=fdopendir(d);if(!dp)return 9;struct dirent*e;int dot=0;while((e=readdir(dp)))if(!strcmp(e->d_name,".")){dot=(uint64_t)e->d_ino==(uint64_t)a.st_ino;break;}if(!dot)return 10;struct statx sx;if(statx(dest,"",AT_EMPTY_PATH,STATX_BASIC_STATS,&sx)||sx.stx_ino!=(uint64_t)a.st_ino||sx.stx_dev_major!=major(a.st_dev)||sx.stx_dev_minor!=minor(a.st_dev))return 11;puts("packed root bind source-fd/dest-fd/path/getdents/statx identity PASS");return 0;}
''')
run('cc','-static','-O2','-Wall','-Wextra','-Werror',root/'root-identity.c','-o',src/'probe-root-identity')
(root/'static.c').write_text('#include <stdio.h>\nint main(void){puts("static pack ELF PASS");return 0;}\n')
run('cc','-static',root/'static.c','-o',src/'static')
run('cc',base/'tests/loader-maps.c','-o',src/'maps')
(root/'lib.c').write_text('int pack_answer(void){return 42;}\n')
(root/'dynamic.c').write_text('#include <stdio.h>\nextern int pack_answer(void);int main(void){if(pack_answer()!=42)return 1;puts("dynamic pack ELF + .so mmap PASS");return 0;}\n')
run('cc','-shared','-fPIC',root/'lib.c','-o',src/'lib/libfixture.so')
run('cc',root/'dynamic.c','-L'+str(src/'lib'),'-lfixture','-Wl,-rpath,/packed/lib','-o',src/'dynamic')
ldconfig=next((p for p in (pathlib.Path('/sbin/ldconfig.real'),pathlib.Path('/sbin/ldconfig'))
               if p.exists() and p.read_bytes()[:4]==b'\x7fELF'
               and b'INTERP' not in output('readelf','-l',p)),None)
if ldconfig:
 (src/'bin').mkdir(exist_ok=True);shutil.copy2(ldconfig,src/'bin/ldconfig')
(src/'script').write_text('#!/bin/sh\nprintf "script from pack PASS\\n"\ncat /packed/data\n');(src/'script').chmod(0o755)
(src/'data').write_bytes(b'0123456789abcdef');(src/'large').write_bytes(bytes(range(256))*8193);(src/'nested/leaf').write_text('nested content\n');(src/'link').symlink_to('nested/leaf')
repo=root/'repo';run('ostree','init','--repo='+str(repo),'--mode=bare-user');run('ostree','commit','--repo='+str(repo),'-b','fixture','--tree=dir='+str(src),'-m','pack host')
rev=output('ostree','rev-parse','--repo='+str(repo),'fixture').decode().strip();bundle=root/'bundle'
run(sys.executable,base.parent/'packs/pack.py','pack','--repo',repo,'--commit',rev,'--out',bundle)
sys.path.insert(0,str(base.parent/'packs'));import pack
rows=pack.read_index(bundle/'objects.index')
for row in rows:
 if row['kind']=='file':
  assert row['offset']%4096==0
  with zipfile.ZipFile(bundle/row['pack']) as z:assert all(i.compress_type==zipfile.ZIP_STORED for i in z.infolist())
proxy=[base/'build/linux-layer-v3','--android-filter','--pack-mount','/packed',bundle/'objects.index',bundle]
def guest(*args):return output(*proxy,*args)
for binary in ('static','dynamic','script','probe','probe-root-dir'):print(guest('/packed/'+binary).decode(),end='\n',flush=True)
identity_source=root/'bind-source';identity_source.mkdir()
print(output(base/'build/linux-layer-v3','--android-filter','--pack-mount','/usr',bundle/'objects.index',bundle,'/usr/probe-root-identity',identity_source,'/usr').decode(),end='',flush=True)
empty=root/'empty-runtime-files';empty.mkdir()
print(output(base/'build/linux-layer-v3','--android-filter','--pack-mount','/usr',bundle/'objects.index',bundle,'--map',empty,'/newroot','/usr/probe-root-dir','/newroot','usr','data').decode(),end='',flush=True)
bind_probe_src=root/'packed-bind-fd.c'
bind_probe_src.write_text(r'''#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc,char **argv){
 if(argc!=2)return 1;
 int source=open(argv[1],O_RDONLY|O_DIRECTORY);struct stat before,after;
 if(source<0||fstat(source,&before))return 2;
 char proc[64];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",source);
 if(mount(proc,"/newroot/usr",NULL,MS_BIND,NULL)){perror("bind packed fd");return 3;}
 if(mount(NULL,"/newroot/usr",NULL,MS_BIND|MS_REMOUNT|MS_RDONLY,NULL))return 11;
 int target=open("/newroot/usr",O_PATH|O_DIRECTORY);
 if(target<0||fstat(target,&after))return 4;
 if(before.st_dev!=after.st_dev||before.st_ino!=after.st_ino){fprintf(stderr,"packed fd bind changed inode\n");return 5;}
 const char *parents[]={"lib","x86_64-linux-gnu","GL","default"};
 for(unsigned i=0;i<sizeof(parents)/sizeof(parents[0]);i++){
  if(mkdirat(target,parents[i],0755)!=-1||errno!=EEXIST){perror("mkdir existing packed parent");return 6;}
  int next=openat(target,parents[i],O_PATH|O_DIRECTORY|O_NOFOLLOW);if(next<0)return 7;close(target);target=next;
 }
 int leaf=openat(target,"extension-marker",O_RDONLY);char text[64];
 const char expected[]="GL extension directory entry\n";
 if(leaf<0||read(leaf,text,sizeof(text))!=(ssize_t)sizeof(expected)-1||memcmp(text,expected,sizeof(expected)-1))return 8;
 if(mkdirat(target,"missing-parent",0755)!=-1||errno!=EROFS)return 9;
 if(fstatat(target,"missing-parent",&after,AT_SYMLINK_NOFOLLOW)!=-1||errno!=ENOENT)return 10;
 close(leaf);close(target);close(source);
 puts("packed bind-fd inode and dirfd-relative existing parents PASS");return 0;
}
''')
bind_probe=root/'packed-bind-fd';run('cc','-static','-O2','-Wall','-Wextra','-Werror',bind_probe_src,'-o',bind_probe)
files=root/'deployment/files';files.mkdir(parents=True)
print(output(base/'build/linux-layer-v3','--android-filter','--pack-mount','/usr',bundle/'objects.index',bundle,'--map',empty,'/newroot',bind_probe,files).decode(),end='',flush=True)
if ldconfig and shutil.which('bwrap'):
 # Flatpak's cache subprocess binds the runtime from an fd, then executes
 # the runtime's real ldconfig. Keep the host bwrap's interpreter/libraries
 # reachable before its pivot; the final root contains only the packed /usr.
 bwrap=root/'bwrap';shutil.copy2(shutil.which('bwrap'),bwrap)
 # Flatpak's runtime /etc bind uses an O_PATH fd for each entry. Reproduce
 # bwrap's bind-fd path and its post-mount st_dev/st_ino equality check.
 print(output(base/'build/linux-layer-v3','--android-filter',
              '--map',os.path.realpath('/lib'),'/lib','--map',os.path.realpath('/lib64'),'/lib64',
              '--pack-mount','/usr',bundle/'objects.index',bundle,
              root/'file-bind-launcher',bwrap).decode(),end='',flush=True)
 print('packed regular-file --ro-bind-fd through bwrap PASS',flush=True)
 cache=root/'ld-so-cache';cache.mkdir()
 config=root/'ld.so.conf';config.write_text('/usr/lib\n')
 runtime_fd=os.open(files,os.O_RDONLY|os.O_DIRECTORY);cache_fd=os.open(cache,os.O_RDONLY|os.O_DIRECTORY)
 args=[base/'build/linux-layer-v3','--android-filter','--trace',
       '--map',os.path.realpath('/lib'),'/lib','--map',os.path.realpath('/lib64'),'/lib64',
       '--pack-mount','/usr',bundle/'objects.index',bundle,bwrap,
       '--ro-bind-fd',str(runtime_fd),'/usr','--symlink','usr/lib','/lib',
       '--bind-fd',str(cache_fd),'/run/ld-so-cache-dir',
       '--ro-bind',config,'/etc/ld.so.conf','--proc','/proc','--dev','/dev',
       '/usr/bin/ldconfig','-X','-C','/run/ld-so-cache-dir/cache']
 try:
  with (root/'ldconfig.stdout').open('wb') as out,(root/'ldconfig.trace').open('wb') as err:
   run(*args,pass_fds=[runtime_fd,cache_fd],stdout=out,stderr=err,timeout=30)
 finally:os.close(runtime_fd);os.close(cache_fd)
 assert (cache/'cache').stat().st_size>0
 assert b'libfixture.so' in output(ldconfig,'-p','-C',cache/'cache')
 print('real packed ldconfig -X -C through bwrap runtime/cache bind-fd PASS',flush=True)
 # Repeat with Flatpak's GL extension topology: tmpfs at GL, then a packed
 # extension at GL/default, with its add-ld-path in the generated config.
 ext_src=root/'ld-extension-src';(ext_src/'lib').mkdir(parents=True)
 shutil.copy2(src/'lib/libfixture.so',ext_src/'lib/libfixture.so')
 ext_repo=root/'ld-extension-repo';run('ostree','init','--repo='+str(ext_repo),'--mode=bare-user')
 run('ostree','commit','--repo='+str(ext_repo),'-b','fixture','--tree=dir='+str(ext_src),'-m','ldconfig extension')
 ext_rev=output('ostree','rev-parse','--repo='+str(ext_repo),'fixture').decode().strip();ext_bundle=root/'ld-extension-bundle'
 run(sys.executable,base.parent/'packs/pack.py','pack','--repo',ext_repo,'--commit',ext_rev,'--out',ext_bundle)
 ext_files=root/'ld-extension-deployment/files';ext_files.mkdir(parents=True)
 gl='/usr/lib/x86_64-linux-gnu/GL';extension=gl+'/default'
 config.write_text(extension+'/lib\n');(cache/'cache').unlink()
 at=args.index(bwrap);args[at:at]=['--pack-mount',extension,ext_bundle/'objects.index',ext_bundle]
 at=args.index('--symlink');args[at:at]=['--tmpfs',gl,'--ro-bind',ext_files,extension]
 runtime_fd=os.open(files,os.O_RDONLY|os.O_DIRECTORY);cache_fd=os.open(cache,os.O_RDONLY|os.O_DIRECTORY)
 args[args.index('--ro-bind-fd')+1]=str(runtime_fd);args[args.index('--bind-fd')+1]=str(cache_fd)
 try:
  with (root/'ldconfig-extension.stdout').open('wb') as out,(root/'ldconfig-extension.trace').open('wb') as err:
   run(*args,pass_fds=[runtime_fd,cache_fd],stdout=out,stderr=err,timeout=30)
 finally:os.close(runtime_fd);os.close(cache_fd)
 assert (extension+'/lib/libfixture.so').encode() in output(ldconfig,'-p','-C',cache/'cache')
 print('real packed ldconfig with GL tmpfs/extension/add-ld-path PASS',flush=True)
 # A file bind below a directory absent from the packed runtime must be
 # visible through readdir on that newly-created parent, and then openable.
 (root/'bind-entry').write_text('newly bound Vulkan ICD entry\n')
 (root/'bind-entry-launcher.c').write_text(r'''#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int main(int argc,char **argv){if(argc!=3)return 1;int usr=open("/usr",O_PATH|O_DIRECTORY);int entry=open(argv[2],O_PATH);if(usr<0||entry<0)return 2;char u[24],f[24];snprintf(u,sizeof(u),"%d",usr);snprintf(f,sizeof(f),"%d",entry);char *args[]={argv[1],"--ro-bind-fd",u,"/usr","--ro-bind-fd",f,"/usr/lib/x86_64-linux-gnu/GL/vulkan/icd.d/maton_icd.json","--proc","/proc","--dev","/dev","/usr/bin/probe-new-bind",NULL};execv(argv[1],args);return 3;}
''')
 run('cc','-static','-O2','-Wall','-Wextra','-Werror',root/'bind-entry-launcher.c','-o',root/'bind-entry-launcher')
 bind_fd=os.open(root/'bind-entry',os.O_PATH)
 runtime_fd=os.open(files,os.O_RDONLY|os.O_DIRECTORY)
 args=[base/'build/linux-layer-v3','--android-filter','--trace',
       '--map',os.path.realpath('/lib'),'/lib','--map',os.path.realpath('/lib64'),'/lib64',
       '--pack-mount','/usr',bundle/'objects.index',bundle,bwrap,
       '--ro-bind-fd',str(runtime_fd),'/usr',
       '--tmpfs','/usr/lib/x86_64-linux-gnu/GL',
       '--ro-bind-fd',str(bind_fd),'/usr/lib/x86_64-linux-gnu/GL/vulkan/icd.d/maton_icd.json',
       '--proc','/proc','--dev','/dev','/usr/bin/probe-new-bind']
 try:
  output(*args,pass_fds=[runtime_fd,bind_fd],timeout=30)
 finally:os.close(runtime_fd);os.close(bind_fd)
 print('packed parent bwrap --ro-bind-fd new-directory regression PASS',flush=True)
else:print('SKIP packed ldconfig: static host ldconfig/bwrap unavailable',flush=True)
for name in ('data','large','link','nested/leaf'):
 data=(src/name).read_bytes();assert guest('/bin/cat','/packed/'+name)==data
 digest=guest('/usr/bin/sha256sum','/packed/'+name).decode().split()[0];assert digest==hashlib.sha256(data).hexdigest()
print('cat + sha256sum (including >1 MiB and symlink) PASS',flush=True)
assert guest('/bin/dd','if=/packed/large','bs=4096','skip=123','count=37','status=none')==(src/'large').read_bytes()[123*4096:160*4096]
assert guest('/bin/dd','if=/packed/large','bs=1','skip=2097400','count=100','status=none')==(src/'large').read_bytes()[2097400:2097500]
print('dd skip/count and EOF PASS',flush=True)
# ls traverses actual virtual directory descriptors. Compare metadata to a real
# OSTree checkout; directory storage size/timestamps/link counts are synthetic.
checkout=root/'checkout';run('ostree','checkout','--force-copy','--repo='+str(repo),rev,checkout)
listing=guest('/bin/ls','-lnR','--time-style=+TIME','/packed').decode();(root/'ls-packed.txt').write_text(listing)
real=output('/bin/ls','-lnR','--time-style=+TIME',checkout).decode();(root/'ls-real.txt').write_text(real)
def ls_rows(text):
 result={};directory=''
 for line in text.splitlines():
  if line.endswith(':'):directory=line[:-1].split('/packed',1)[-1] if '/packed' in line else line[:-1].split(str(checkout),1)[-1]
  elif line[:1] in '-dl' and len(line.split())>=7:
   v=line.split();result[directory+'/'+v[6]]=(v[0],v[2],v[3],None if v[0][0]=='d' else v[4],v[7:])
 return result
assert ls_rows(listing)==ls_rows(real),(ls_rows(listing),ls_rows(real))
print('ls -lnR modes/uid/gid/file sizes/symlinks vs real checkout PASS',flush=True)
scaffold=root/'scaffold';scaffold.mkdir()
for name in ('usr','bin','lib','lib64','etc','sbin','proc','dev'):(scaffold/name).symlink_to('/'+name)
assert b'static pack ELF PASS' in output(base/'build/linux-layer-v3','--android-filter','--root',scaffold,'--pack-mount','/packed',bundle/'objects.index',bundle,'/packed/static')
print('pack mount overlays an existing --root scaffold PASS',flush=True)
other=root/'other-src';(other/'share').mkdir(parents=True);(other/'share'/'unrelated').write_text('another /usr pack\n')
other_repo=root/'other-repo';run('ostree','init','--repo='+str(other_repo),'--mode=bare-user');run('ostree','commit','--repo='+str(other_repo),'-b','fixture','--tree=dir='+str(other),'-m','other /usr pack')
other_rev=output('ostree','rev-parse','--repo='+str(other_repo),'fixture').decode().strip();other_bundle=root/'other-bundle'
run(sys.executable,base.parent/'packs/pack.py','pack','--repo',other_repo,'--commit',other_rev,'--out',other_bundle)
print(output(base/'build/linux-layer-v3','--android-filter','--pack-mount','/usr',bundle/'objects.index',bundle,'--pack-mount','/usr',other_bundle/'objects.index',other_bundle,'--map',empty,'/newroot','/newroot/usr/probe-dir-mount').decode(),end='',flush=True)
if sys.platform.startswith('linux'):
 app_src=root/'exec-app';(app_src/'bin').mkdir(parents=True)
 asm=root/'exec-packed.S'
 asm.write_text(''' .global _start
.text
_start:
 mov $1,%rax
 mov $1,%rdi
 lea message(%rip),%rsi
 mov $27,%rdx
 syscall
 mov $60,%rax
 xor %rdi,%rdi
 syscall
.section .rodata
message:.ascii "exec-packed PT_INTERP PASS\\n"
''')
 obj=root/'exec-packed.o';exe=app_src/'bin/x'
 run('as','--64',asm,'-o',obj)
 run('ld','-pie','-dynamic-linker','/lib64/ld-linux-x86-64.so.2','-e','_start','-o',exe,obj)
 assert b'/lib64/ld-linux-x86-64.so.2' in output('readelf','-l',exe)
 script=app_src/'bin/script';script.write_text('#!/app/bin/x\n');script.chmod(0o755)
 app_repo=root/'exec-app-repo';run('ostree','init','--repo='+str(app_repo),'--mode=bare-user');run('ostree','commit','--repo='+str(app_repo),'-b','fixture','--tree=dir='+str(app_src),'-m','packed exec app')
 app_rev=output('ostree','rev-parse','--repo='+str(app_repo),'fixture').decode().strip();app_bundle=root/'exec-app-bundle'
 run(sys.executable,base.parent/'packs/pack.py','pack','--repo',app_repo,'--commit',app_rev,'--out',app_bundle)
 runtime_src=root/'exec-runtime-deploy'/'files';(runtime_src/'bin').mkdir(parents=True);(runtime_src/'usr/lib64').mkdir(parents=True)
 (runtime_src/'lib64').symlink_to('usr/lib64')
 linker=runtime_src/'usr/lib64/ld-linux-x86-64.so.2';shutil.copyfile(os.path.realpath('/lib64/ld-linux-x86-64.so.2'),linker);linker.chmod(0o755)
 runtime_repo=root/'exec-runtime-repo';run('ostree','init','--repo='+str(runtime_repo),'--mode=bare-user');run('ostree','commit','--repo='+str(runtime_repo),'-b','fixture','--tree=dir='+str(runtime_src),'-m','packed exec runtime')
 runtime_rev=output('ostree','rev-parse','--repo='+str(runtime_repo),'fixture').decode().strip();runtime_bundle=root/'exec-runtime-bundle'
 run(sys.executable,base.parent/'packs/pack.py','pack','--repo',runtime_repo,'--commit',runtime_rev,'--out',runtime_bundle)
 probe_src=root/'packed-path-probe.c'
 probe_src.write_text(r'''#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <linux/stat.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc,char **argv){if(argc!=3)return 2;int dir=open(argv[1],O_PATH|O_NOFOLLOW|O_CLOEXEC|O_DIRECTORY);struct statx sx;if(dir<0||statx(dir,"",AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,STATX_TYPE|STATX_INO|STATX_MNT_ID,&sx)||!S_ISDIR(sx.stx_mode)||(sx.stx_mask&STATX_MNT_ID)==0||sx.stx_mnt_id==0)return 3;char proc[64];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",dir);int dup=open(proc,O_PATH|O_CLOEXEC|O_DIRECTORY);if(dup<0||statx(dup,"",AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,STATX_TYPE,&sx)||!S_ISDIR(sx.stx_mode))return 6;int link=open(argv[2],O_PATH|O_NOFOLLOW|O_CLOEXEC);char target[64];if(link<0||statx(link,"",AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,STATX_TYPE|STATX_INO|STATX_MNT_ID,&sx)||!S_ISLNK(sx.stx_mode)||(sx.stx_mask&STATX_MNT_ID)==0||sx.stx_mnt_id==0)return 4;ssize_t n=readlinkat(link,"",target,sizeof(target));if(n!=9||memcmp(target,"usr/lib64",9))return 5;puts("PASS physical deployment files/ O_PATH directory/symlink chase through pack index");return 0;}
''')
 path_probe=root/'packed-path-probe';run('cc','-static','-O2','-Wall','-Wextra','-Werror',probe_src,'-o',path_probe)
 print(output(base/'build/linux-layer-v3','--android-filter','--pack-mount','/usr',runtime_bundle/'objects.index',runtime_bundle,path_probe,runtime_src/'bin',runtime_src/'lib64').decode(),end='',flush=True)
 harness_src=root/'pivot-exec.c'
 harness_src.write_text(r'''#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/stat.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <unistd.h>
extern char **environ;
int main(int argc,char **argv){if(argc!=3)return 2;char app[4096],usr[4096],old[4096];snprintf(app,sizeof(app),"%s/app",argv[1]);snprintf(usr,sizeof(usr),"%s/usr",argv[1]);snprintf(old,sizeof(old),"%s/oldroot",argv[1]);if(mount("/app",app,NULL,MS_BIND,NULL)||mount("/usr",usr,NULL,MS_BIND,NULL)||syscall(SYS_pivot_root,argv[1],old)||chdir("/")){perror("packed pivot setup");return 3;}int dir=openat(AT_FDCWD,"/usr/bin",O_PATH|O_NOFOLLOW|O_CLOEXEC);struct statx sx;if(dir<0||statx(dir,"",AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,STATX_TYPE,&sx)||!S_ISDIR(sx.stx_mode))return 5;int link=openat(AT_FDCWD,"/usr/lib64",O_PATH|O_NOFOLLOW|O_CLOEXEC);char target[64];if(link<0||statx(link,"",AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,STATX_TYPE,&sx)||!S_ISLNK(sx.stx_mode))return 6;ssize_t n=readlinkat(link,"",target,sizeof(target));if(n!=9||memcmp(target,"usr/lib64",9))return 7;char *args[]={argv[2],NULL};execve(argv[2],args,environ);perror("packed pivot execve");return 4;}
''')
 harness=root/'pivot-exec';run('cc','-static','-O2','-Wall','-Wextra','-Werror',harness_src,'-o',harness)
 pivot_root=root/'pivot-root'
 for name in ('app','usr','oldroot'):(pivot_root/name).mkdir(parents=True,exist_ok=True)
 # Match Flatpak's bwrap --symlink usr/bin /bin and usr/lib64 /lib64 setup.
 (pivot_root/'bin').symlink_to('usr/bin');(pivot_root/'lib64').symlink_to('usr/lib64')
 exec_proxy=[base/'build/linux-layer-v3','--android-filter','--pack-mount','/app',app_bundle/'objects.index',app_bundle,'--pack-mount','/usr',runtime_bundle/'objects.index',runtime_bundle]
 for target in ('/app/bin/x','/app/bin/script'):
  result=output(*exec_proxy,harness,pivot_root,target).decode()
  assert result=='exec-packed PT_INTERP PASS\n',(target,result)
 print('PASS packed O_PATH directory/symlink chase and readlinkat(fd, "") plus ELF/shebang after emulated bind/pivot; PT_INTERP loaded from /usr pack',flush=True)
print('ALL pack host checks PASS',flush=True)
