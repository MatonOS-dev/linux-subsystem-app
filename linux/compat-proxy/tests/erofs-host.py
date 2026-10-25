#!/usr/bin/env python3
"""Exercise MTPKIDX2 metadata with ordinary directories standing in for mounts."""
import hashlib, pathlib, shutil, struct, subprocess

base=pathlib.Path(__file__).resolve().parents[1]
root=base/'build/erofs-host';shutil.rmtree(root,ignore_errors=True);root.mkdir(parents=True)
src=root/'fixture.c'
src.write_text(r'''#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc,char **argv){
 if(argc>1&&!strcmp(argv[1],"exec")){puts("EROFS EXEC PASS");return 0;}
 struct stat st;
 /* A Flatpak deployment keeps an empty files/ directory beside the index
  * and layer mounts. It must act as the indexed root, for both ordinary and
  * O_PATH descriptors, including the open_tree fallback's empty-path stat. */
 char deployment[4096];assert(snprintf(deployment,sizeof(deployment),"%s/flatpak-launch/system/runtime/org.freedesktop.Platform/x86_64/25.08/commit",argv[1])>0);
 int commit=open(deployment,O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(commit>=0);
 if(argc>2&&!strcmp(argv[2],"unassociated")) {
  int empty=openat(commit,"files",O_PATH|O_DIRECTORY|O_CLOEXEC);assert(empty>=0);
  int absent=openat(empty,"sample",O_RDONLY|O_CLOEXEC);assert(absent<0);
  close(empty);close(commit);puts("EROFS BACKING ASSOCIATION CONTROL PASS");return 0;
 }
 int pathfiles=openat(commit,"files",O_PATH|O_DIRECTORY|O_CLOEXEC);assert(pathfiles>=0);
 assert(!fstatat(pathfiles,"",&st,AT_EMPTY_PATH)&&S_ISDIR(st.st_mode));
 int relative=openat(pathfiles,"sample",O_RDONLY|O_CLOEXEC);assert(relative>=0);
 char b[13]={0};assert(read(relative,b,12)==12&&!strcmp(b,"hello erofs\n"));close(relative);close(pathfiles);
 int files=openat(commit,"files",O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(files>=0);
 assert(!fstat(files,&st)&&S_ISDIR(st.st_mode));
 int saw_indexed=0;DIR *rootdir=fdopendir(dup(files));assert(rootdir);struct dirent *entry;
 while((entry=readdir(rootdir)))saw_indexed|=!strcmp(entry->d_name,"sample");
 closedir(rootdir);assert(saw_indexed);
 relative=openat(files,"sample",O_RDONLY|O_CLOEXEC);assert(relative>=0);
 memset(b,0,sizeof(b));assert(read(relative,b,12)==12&&!strcmp(b,"hello erofs\n"));close(relative);
 close(files);
 int pathroot=openat(commit,"files",O_PATH|O_DIRECTORY|O_CLOEXEC);assert(pathroot>=0);
 assert(!fstatat(pathroot,"",&st,AT_EMPTY_PATH)&&S_ISDIR(st.st_mode));
 assert(!fstatat(pathroot,"",&st,AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW)&&S_ISDIR(st.st_mode));
 relative=openat(pathroot,"lib/x",O_RDONLY|O_CLOEXEC);assert(relative>=0);
 assert(!fstat(relative,&st)&&st.st_size==5);close(relative);close(pathroot);close(commit);
 assert(!stat("/usr/sample",&st));assert(st.st_size==12&&(st.st_mode&0777)==0644);
 assert(lstat("/usr/link",&st)==0&&S_ISLNK(st.st_mode));
 char link[64];assert(readlink("/usr/link",link,sizeof(link))==6&&!memcmp(link,"sample",6));
 assert(stat("/usr/missing",&st)<0);
 DIR *d=opendir("/usr");assert(d);int saw_sample=0,saw_sub=0,saw_probe=0;struct dirent *de;
 while((de=readdir(d))) {saw_sample|=!strcmp(de->d_name,"sample");saw_sub|=!strcmp(de->d_name,"sub");saw_probe|=!strcmp(de->d_name,"probe");}
 closedir(d);assert(saw_sample&&saw_sub&&saw_probe);
 int fd=open("/usr/sample",O_RDONLY);assert(fd>=0);memset(b,0,sizeof(b));assert(read(fd,b,12)==12&&!strcmp(b,"hello erofs\n"));
 void *p=mmap(NULL,12,PROT_READ,MAP_PRIVATE,fd,0);assert(p!=MAP_FAILED&&!memcmp(p,"hello erofs\n",12));munmap(p,12);
 assert(!fstat(fd,&st)&&st.st_size==12);close(fd);
 puts("EROFS INDEX/OPEN/MMAP PASS");return 0;
}''')
probe=root/'probe';subprocess.run(['cc','-static','-O2','-Wall','-Wextra','-Werror',src,'-o',probe],check=True)
split='pack_'+'1'*64
layer=root/split; (layer/'sub').mkdir(parents=True,exist_ok=True)
(layer/'sample').write_bytes(b'hello erofs\n');(layer/'probe').write_bytes(probe.read_bytes());(layer/'probe').chmod(0o755)
(layer/'sub'/'nested').write_text('nested\n');(layer/'link').symlink_to('sample')
(root/'flatpak-launch'/'system'/'runtime'/'org.freedesktop.Platform'/'x86_64'/'25.08'/'commit'/'files').mkdir(parents=True)
(layer/'lib').mkdir(exist_ok=True);(layer/'lib'/'x').write_text('index')
records=[
 ('/lib','f'*64,'dir','',0xffffffff,0,1,0o040755),
 ('/lib/x','1'*64,'file','',0,5,1,0o100644),
 ('/link','a'*64,'symlink','sample',0xffffffff,6,1,0o120777),
 ('/probe','b'*64,'file','',0,probe.stat().st_size,1,0o100755),
 ('/sample','c'*64,'file','',0,12,1,0o100644),
 ('/sub','d'*64,'dir','',0xffffffff,0,1,0o040755),
 ('/sub/nested','e'*64,'file','',0,7,1,0o100644),
]
idx=bytearray(b'MTPKIDX2'+struct.pack('<I',1))
def string(s):
 b=s.encode();return struct.pack('<I',len(b))+b
idx+=string(split)+string('assets/matonos/pack.erofs')+struct.pack('<Q',1)+bytes.fromhex('1'*64)
idx+=struct.pack('<I',len(records))
for path,checksum,kind,target,layer_id,size,mtime,mode in records:
 for s in (path,checksum,kind,target):idx+=string(s)
 idx+=struct.pack('<I',0)+struct.pack('<IQQIIII',layer_id,size,mtime,1000,1000,mode,0)
index=root/'layers.index';index.write_bytes(idx)
empty_index=root/'empty.index'
empty_index.write_bytes(b'MTPKIDX2'+struct.pack('<I',1)+string(split)+string('assets/matonos/pack.erofs')+struct.pack('<Q',1)+bytes.fromhex('1'*64)+struct.pack('<I',0))
cmd=[base/'build/linux-layer-v3','--android-filter','--erofs-mount','/usr',index,root,'--erofs-backing','/usr',root/'flatpak-launch'/'system'/'runtime'/'org.freedesktop.Platform'/'x86_64'/'25.08'/'commit'/'files',probe]
targets=['/app','/usr/lib/x86_64-linux-gnu/GL','/usr/lib/x86_64-linux-gnu/GL','/usr/share/runtime/locale','/usr/lib/x86_64-linux-gnu/codecs-extra']
for n,target in enumerate(targets):
    files=root/'flatpak-launch'/f'package-{n}'/'commit'/'files';files.mkdir(parents=True,exist_ok=True)
    package_index=empty_index if target=='/usr/share/runtime/locale' else index
    cmd[-1:-1]=['--erofs-mount',target,package_index,root,'--erofs-backing',target,files]
unassociated=[base/'build/linux-layer-v3','--android-filter','--erofs-mount','/usr',index,root,probe,root,'unassociated']
run=subprocess.run(list(map(str,unassociated)),check=True,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
assert 'EROFS BACKING ASSOCIATION CONTROL PASS' in run.stdout,run.stdout+run.stderr
run=subprocess.run(list(map(str,cmd+[root])),text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
assert run.returncode==0,run.stdout+run.stderr
assert 'EROFS INDEX/OPEN/MMAP PASS' in run.stdout,run.stdout+run.stderr
run=subprocess.run(list(map(str,cmd+['exec'])),check=True,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
assert 'EROFS EXEC PASS' in run.stdout,run.stdout+run.stderr
legacy=root/'legacy.index';legacy.write_bytes(b'MTPKIDX1'+struct.pack('<I',0))
rejected=subprocess.run(list(map(str,[base/'build/linux-layer-v3','--android-filter','--erofs-mount','/usr',legacy,root,probe])),text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
assert rejected.returncode!=0,'proxy accepted removed MTPKIDX1 format'
print('PASS MTPKIDX2 index stat/readdir/readlink/open/mmap/exec/miss')
