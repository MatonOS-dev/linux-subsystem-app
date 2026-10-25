#!/usr/bin/env python3
"""Exercise MTPKIDX2 initial and update output with real OSTree commits."""
import hashlib
import os
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from erofs_layers import read_index

ROOT = pathlib.Path(__file__).resolve().parents[1]
MKFS = os.environ.get('MKFS_EROFS', str(ROOT/'prebuilt/x86_64/matonos-mkfs-erofs'))

def run(*args): subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

with tempfile.TemporaryDirectory(prefix='matonos-erofs-layers-test-') as td:
    t=pathlib.Path(td); repo=t/'repo'; src=t/'src'
    (src/'usr/bin').mkdir(parents=True); (src/'usr/share/doc').mkdir(parents=True)
    (src/'usr/bin/tool').write_bytes(b'unchanged payload')
    (src/'usr/share/doc/change').write_bytes(b'old payload')
    (src/'usr/share/doc/delete').write_bytes(b'delete me')
    (src/'usr/share/doc/file-link').write_bytes(b'old link file')
    (src/'usr/share/doc/deleted-dir').mkdir(); (src/'usr/share/doc/deleted-dir/child').write_text('gone')
    (src/'usr/share/doc/link').symlink_to('../bin/tool')
    run('ostree','init','--repo='+str(repo),'--mode=bare-user')
    def commit(msg):
        run('ostree','commit','--repo='+str(repo),'-b','test','--tree=dir='+str(src),'-m',msg)
        return subprocess.check_output(['ostree','rev-parse','--repo='+str(repo),'test'],text=True).strip()
    a=commit('A'); first=t/'A'
    run(sys.executable,str(ROOT/'erofs_layers.py'),'--repo',str(repo),'--commit',a,'--out',str(first),'--mkfs',MKFS)
    layer_a,rows_a=read_index(first/'layers.index')
    assert '/usr/bin/tool' in {r['path'] for r in rows_a}
    old_hashes={x['sha256'] for x in layer_a}
    (src/'usr/share/doc/change').write_bytes(b'new payload')
    (src/'usr/share/doc/new').write_bytes(b'added')
    (src/'usr/share/doc/delete').unlink()
    (src/'usr/share/doc/deleted-dir/child').unlink(); (src/'usr/share/doc/deleted-dir').rmdir()
    (src/'usr/share/doc/file-link').unlink(); (src/'usr/share/doc/file-link').symlink_to('../bin/tool')
    b=commit('B'); second=t/'B'
    run(sys.executable,str(ROOT/'erofs_layers.py'),'--repo',str(repo),'--commit',b,'--out',str(second),
        '--mkfs',MKFS,'--old-index',str(first/'layers.index'),'--old-layers',str(first))
    layer_b,rows_b=read_index(second/'layers.index'); paths={r['path']:r for r in rows_b}
    assert '/usr/share/doc/delete' not in paths and '/usr/share/doc/deleted-dir/child' not in paths
    assert paths['/usr/share/doc/file-link']['kind']=='symlink'
    assert paths['/usr/share/doc/change']['kind']=='file' and paths['/usr/share/doc/new']['kind']=='file'
    assert old_hashes.intersection(x['sha256'] for x in layer_b)
    newly_written=[x for x in layer_b if x['sha256'] not in old_hashes]
    assert len(newly_written)==1, newly_written
    for x in layer_b:
        image=second/('pack-'+x['sha256']+'.erofs')
        assert image.is_file() and image.stat().st_size==x['image_size'] and sha(image)==x['sha256']
    print(f"EROFS layer A→B passed: A={len(layer_a)} layers/{sum(x['image_size'] for x in layer_a)} bytes, "
          f"B={len(layer_b)} layers/{sum(x['image_size'] for x in layer_b)} bytes, "
          f"new={newly_written[0]['image_size']} bytes; mkfs tool={MKFS}")
