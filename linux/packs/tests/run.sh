#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1],ignore_errors=True)" "$tmp"' EXIT
export TMP_PACK_TEST="$tmp"
python3 - <<'PY'
import os,pathlib,subprocess,sys,json,zipfile,stat,shutil
root=pathlib.Path(os.environ['TMP_PACK_TEST']); repo=root/'repo'; src=root/'src'; src.mkdir()
def run(*a): subprocess.run(a,check=True,stdout=subprocess.PIPE)
run('ostree','init','--repo='+str(repo),'--mode=bare-user')
def commit(ref):
 run('ostree','commit','--repo='+str(repo),'-b','test','--tree=dir='+str(src),'-m',ref)
 return subprocess.check_output(['ostree','rev-parse','--repo='+str(repo),'test'],text=True).strip()
(src/'usr/share/doc/nested').mkdir(parents=True)
(src/'usr/bin').mkdir(parents=True)
(src/'usr/bin/tool').write_text('#!/bin/sh\necho one\n');(src/'usr/bin/tool').chmod(0o755)
(src/'usr/share/doc/nested/a').write_bytes(b'a'*8192);(src/'usr/share/doc/link').symlink_to('nested/a')
c1=commit('one')
(src/'usr/bin/tool').write_text('#!/bin/sh\necho two\n');(src/'usr/share/doc/nested/a').write_bytes(b'b'*8192)
(src/'usr/share/doc/renamed').write_text('rename');(src/'usr/share/doc/nested/old').write_text('deleted');(src/'usr/share/doc/nested/old').unlink()
c2=commit('two')
(src/'usr/bin/new').write_text('new');(src/'usr/share/doc/link').unlink();(src/'usr/share/doc/link').symlink_to('../usr/bin/new')
(src/'usr/share/doc/nested/a').unlink();c3=commit('three')
commits=[c1,c2,c3]

def checkout(rev,dest): run('ostree','checkout','--force-copy','--repo='+str(repo),rev,str(dest))
def read_index(path):
 import pack
 return pack.read_index(path)
def reconstruct(bundle,dest):
 rows=read_index(bundle/'objects.index'); dest.mkdir(parents=True)
 for r in sorted(rows,key=lambda x:(x['path'].count('/'),x['path'])):
  p=dest/r['path'].lstrip('/')
  if r['kind']=='dir':p.mkdir(parents=True,exist_ok=True)
  elif r['kind']=='symlink':p.parent.mkdir(parents=True,exist_ok=True);p.symlink_to(r['target'])
  else:
   p.parent.mkdir(parents=True,exist_ok=True)
   with zipfile.ZipFile(bundle/r['pack']) as z: p.write_bytes(z.read(r['checksum']))
   p.chmod(stat.S_IMODE(r['mode']))
 # Apply directory modes only after reconstruction has created every child.
 for r in sorted(rows,key=lambda x:len(x['path']),reverse=True):
  if r['kind']=='dir':(dest/r['path'].lstrip('/')).chmod(stat.S_IMODE(r['mode']))

def verify(rev,bundle):
 expected=root/'expected';actual=root/'actual'
 checkout(rev,expected);reconstruct(bundle,actual)
 rows=read_index(bundle/'objects.index')
 modes={r['path']:stat.S_IMODE(r['mode']) for r in rows}
 for p in expected.rglob('*'):
  rel=p.relative_to(expected);q=actual/rel; a=p.lstat();b=q.lstat()
  assert stat.S_IFMT(a.st_mode)==stat.S_IFMT(b.st_mode),(rel,a.st_mode,b.st_mode)
  assert stat.S_IMODE(a.st_mode)==stat.S_IMODE(b.st_mode),(rel,oct(a.st_mode),oct(b.st_mode))
  assert stat.S_IMODE(b.st_mode)==modes['/'+rel.as_posix()]
  if p.is_symlink(): assert os.readlink(p)==os.readlink(q)
  elif p.is_file(): assert p.read_bytes()==q.read_bytes()
 shutil.rmtree(expected);shutil.rmtree(actual)
import pack
bundles=[]
for n,rev in enumerate(commits,1):
 if n==1:
  out=root/'b1'; subprocess.run([sys.executable,'pack.py','pack','--repo',str(repo),'--commit',rev,'--out',str(out),'--pack-size','10000'],check=True)
 else:
  prior=bundles[-1];out=root/f'b{n}'
  # replay update incrementally using latest self-contained bundle
  subprocess.run([sys.executable,'pack.py','update','--repo',str(repo),'--commit',rev,'--old-index',str(prior/'objects.index'),'--old-packs',str(prior),'--out',str(out),'--dead-ratio','.5'],check=True)
 bundles.append(out);verify(rev,out)
 assert (out/'objects.ostree').exists()
# Replay each policy sequentially across commits 1 -> 2 -> 3.
for ratio in ('.5','.75','0'):
 prior=bundles[0];written=changed=0
 for n,rev in enumerate(commits[1:],2):
  out=root/('policy'+ratio.replace('.','')+'-'+str(n))
  result=json.loads(subprocess.check_output([sys.executable,'pack.py','update','--repo',str(repo),'--commit',rev,'--old-index',str(prior/'objects.index'),'--old-packs',str(prior),'--out',str(out),'--dead-ratio',ratio],text=True))
  written+=result['bytes_written'];changed+=result['bytes_changed'];prior=out
 print('policy',ratio,json.dumps({'bytes_written':written,'bytes_changed':changed,'write_amplification':written/changed if changed else 0},sort_keys=True))
PY
cc -std=c99 -Wall -Wextra -Werror -I. reader.c tests/reader_test.c -o "$tmp/reader-test"
"$tmp/reader-test" "$tmp/b1/objects.index"
echo 'OSTree object-pack tests passed (three real bare-user commits, checkout reconstruction and update-policy replay)'
python3 tests/fsck-app-seccomp-host.py
