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
int main(int argc,char **argv){(void)argv;
 if(argc>1){puts("EROFS EXEC PASS");return 0;}
 struct stat st;assert(!stat("/packed/sample",&st));assert(st.st_size==12&&(st.st_mode&0777)==0644);
 assert(lstat("/packed/link",&st)==0&&S_ISLNK(st.st_mode));
 char link[64];assert(readlink("/packed/link",link,sizeof(link))==6&&!memcmp(link,"sample",6));
 assert(stat("/packed/missing",&st)<0);
 DIR *d=opendir("/packed");assert(d);int saw_sample=0,saw_sub=0,saw_probe=0;struct dirent *de;
 while((de=readdir(d))) {saw_sample|=!strcmp(de->d_name,"sample");saw_sub|=!strcmp(de->d_name,"sub");saw_probe|=!strcmp(de->d_name,"probe");}
 closedir(d);assert(saw_sample&&saw_sub&&saw_probe);
 int fd=open("/packed/sample",O_RDONLY);assert(fd>=0);char b[13]={0};assert(read(fd,b,12)==12&&!strcmp(b,"hello erofs\n"));
 void *p=mmap(NULL,12,PROT_READ,MAP_PRIVATE,fd,0);assert(p!=MAP_FAILED&&!memcmp(p,"hello erofs\n",12));munmap(p,12);
 assert(!fstat(fd,&st)&&st.st_size==12);close(fd);
 puts("EROFS INDEX/OPEN/MMAP PASS");return 0;
}''')
probe=root/'probe';subprocess.run(['cc','-static','-O2','-Wall','-Wextra','-Werror',src,'-o',probe],check=True)
split='pack_'+'1'*64
layer=root/split; (layer/'sub').mkdir(parents=True,exist_ok=True)
(layer/'sample').write_bytes(b'hello erofs\n');(layer/'probe').write_bytes(probe.read_bytes());(layer/'probe').chmod(0o755)
(layer/'sub'/'nested').write_text('nested\n');(layer/'link').symlink_to('sample')
records=[
 ('/','', 'dir','',0xffffffff,0,0,0o040755),
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
cmd=[base/'build/linux-layer-v3','--android-filter','--erofs-mount','/packed',index,root,probe]
run=subprocess.run(list(map(str,cmd)),check=True,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
assert 'EROFS INDEX/OPEN/MMAP PASS' in run.stdout,run.stdout+run.stderr
run=subprocess.run(list(map(str,cmd[:-1]+[probe,'exec'])),check=True,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
assert 'EROFS EXEC PASS' in run.stdout,run.stdout+run.stderr
legacy=root/'legacy.index';legacy.write_bytes(b'MTPKIDX1'+struct.pack('<I',0))
rejected=subprocess.run(list(map(str,[base/'build/linux-layer-v3','--android-filter','--erofs-mount','/packed',legacy,root,probe])),text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
assert rejected.returncode!=0,'proxy accepted removed MTPKIDX1 format'
print('PASS MTPKIDX2 index stat/readdir/readlink/open/mmap/exec/miss')
