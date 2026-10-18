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
 runtime_src=root/'exec-runtime-src';(runtime_src/'bin').mkdir(parents=True);(runtime_src/'usr/lib64').mkdir(parents=True)
 (runtime_src/'lib64').symlink_to('usr/lib64')
 linker=runtime_src/'usr/lib64/ld-linux-x86-64.so.2';shutil.copyfile(os.path.realpath('/lib64/ld-linux-x86-64.so.2'),linker);linker.chmod(0o755)
 runtime_repo=root/'exec-runtime-repo';run('ostree','init','--repo='+str(runtime_repo),'--mode=bare-user');run('ostree','commit','--repo='+str(runtime_repo),'-b','fixture','--tree=dir='+str(runtime_src),'-m','packed exec runtime')
 runtime_rev=output('ostree','rev-parse','--repo='+str(runtime_repo),'fixture').decode().strip();runtime_bundle=root/'exec-runtime-bundle'
 run(sys.executable,base.parent/'packs/pack.py','pack','--repo',runtime_repo,'--commit',runtime_rev,'--out',runtime_bundle)
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
