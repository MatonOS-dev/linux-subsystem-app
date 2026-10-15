#!/usr/bin/env python3
"""Required real OSTree -> aligned ZIP -> resident proxy host acceptance."""
import os,pathlib,subprocess,sys,shutil,hashlib,json,zipfile
base=pathlib.Path(__file__).resolve().parents[1]; root=base/'build/pack-host'
if root.exists():shutil.rmtree(root)
root.mkdir();src=root/'src';src.mkdir();(src/'nested').mkdir();(src/'lib').mkdir()
def run(*args,**kw):return subprocess.run(list(map(str,args)),check=True,**kw)
def output(*args):return subprocess.check_output(list(map(str,args)))
run('cc','-m32','-nostdlib','-static','-no-pie',base/'tests/pack-peer.S','-o',src/'peer')
run('cc','-static','-O2','-Wall','-Wextra','-Werror',base/'tests/pack-vfd.c','-o',src/'probe')
(root/'static.c').write_text('#include <stdio.h>\nint main(void){puts("static pack ELF PASS");return 0;}\n')
run('cc','-static',root/'static.c','-o',src/'static')
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
for binary in ('static','dynamic','script','probe'):print(guest('/packed/'+binary).decode(),end='\n',flush=True)
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
print('ALL pack host checks PASS',flush=True)
