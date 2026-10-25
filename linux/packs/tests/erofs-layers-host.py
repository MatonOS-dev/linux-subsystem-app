#!/usr/bin/env python3
"""Exercise MTPKIDX2 initial and update output with real OSTree commits."""
import hashlib
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import gi
gi.require_version('GLib','2.0')
from gi.repository import GLib
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from erofs_layers import read_index

ROOT = pathlib.Path(__file__).resolve().parents[1]
MKFS = os.environ.get('MKFS_EROFS', str(ROOT/'prebuilt/x86_64/matonos-mkfs-erofs'))
FSCK = os.environ.get('FSCK_EROFS', 'fsck.erofs')

def run(*args): subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def run(*args): subprocess.run(args, check=True, stdout=subprocess.DEVNULL)

def attrs_from_variant(data):
    if not data: return {}
    value=GLib.Variant.new_from_bytes(GLib.VariantType.new('a(ayay)'),GLib.Bytes.new(data),False)
    return {bytes(k).rstrip(b'\0').decode():bytes(v) for k,v in value.unpack()}

def extracted_attrs(path):
    try: raw=os.getxattr(path,'user.ostreemeta',follow_symlinks=False)
    except OSError:
        try: return {k:os.getxattr(path,k,follow_symlinks=False) for k in os.listxattr(path,follow_symlinks=False)}
        except OSError: return {}
    decoded=attrs_from_variant(raw[12:])
    if decoded=={'':b''}: raise AssertionError('cannot decode user.ostreemeta: '+raw.hex())
    return decoded

def compare_tree(expected, actual, rows):
    by_path={r['path']:r for r in rows}
    for base,dirs,files in os.walk(expected,topdown=True,followlinks=False):
        symlink_dirs=[d for d in dirs if (pathlib.Path(base)/d).is_symlink()]
        dirs[:]=[d for d in dirs if d not in symlink_dirs]
        for name in dirs+files+symlink_dirs:
            src=pathlib.Path(base)/name; rel='/'+src.relative_to(expected).as_posix(); dst=actual/rel.lstrip('/')
            assert dst.exists() or dst.is_symlink(), f'missing extracted path {rel}'
            a=src.lstat(); b=dst.lstat(); record=by_path[rel]
            assert stat.S_IFMT(a.st_mode)==stat.S_IFMT(b.st_mode), (rel,a.st_mode,b.st_mode)
            assert stat.S_IMODE(b.st_mode)==stat.S_IMODE(record['mode']), (rel,oct(b.st_mode),oct(record['mode']))
            assert b.st_uid==record['uid'] and b.st_gid==record['gid'], (rel,b.st_uid,b.st_gid,record['uid'],record['gid'])
            if stat.S_ISLNK(a.st_mode): assert os.readlink(src)==os.readlink(dst), rel
            elif stat.S_ISREG(a.st_mode): assert src.read_bytes()==dst.read_bytes(), rel
            ax={k:os.getxattr(src,k,follow_symlinks=False) for k in os.listxattr(src,follow_symlinks=False)
                if k!='user.ostreemeta'}
            bx=attrs_from_variant(bytes.fromhex(record['xattrs']))
            assert ax==bx, (rel,ax,bx)
            if not stat.S_ISLNK(a.st_mode): assert extracted_attrs(dst)==ax, (rel,ax,extracted_attrs(dst))
    indexed={r['path'] for r in rows if r['path']!='/' }
    actual_paths={'/'+p.relative_to(actual).as_posix() for p in actual.rglob('*')}
    assert actual_paths==indexed, (actual_paths-indexed,indexed-actual_paths)

def reconstruct(bundle, index, expected, scratch):
    layers,rows=index; scratch.mkdir()
    extracted=[]
    for n,layer in enumerate(layers):
        image=bundle/('pack-'+layer['sha256']+'.erofs'); destination=scratch/f'layer-{n}'
        destination.mkdir()
        run(FSCK,'--extract='+str(destination),'--preserve','--xattrs',str(image))
        extracted.append(destination)
    actual=scratch/'merged'; actual.mkdir()
    def set_extracted_xattrs(source,dest):
        try:
            for key in os.listxattr(source,follow_symlinks=False):
                os.setxattr(dest,key,os.getxattr(source,key,follow_symlinks=False),follow_symlinks=False)
        except OSError: pass
    # Copy only indexed leaf entries from their owning layer. Directories are
    # created from index metadata after file and symlink payloads are placed.
    for row in sorted(rows,key=lambda r:(r['path'].count('/'),r['path'].encode())):
        dest=actual if row['path']=='/' else actual/row['path'].lstrip('/')
        if row['kind']=='dir': dest.mkdir(parents=True,exist_ok=True); continue
        dest.parent.mkdir(parents=True,exist_ok=True)
        if row['kind']=='symlink':
            source=extracted[row['layer']]/row['path'].lstrip('/')
            assert source.is_symlink() and os.readlink(source)==row['target'], (row['path'],row['layer'])
            dest.symlink_to(row['target'])
        else:
            source=extracted[row['layer']]/row['path'].lstrip('/')
            shutil.copyfile(source,dest)
            set_extracted_xattrs(source,dest)
        if row['kind']!='symlink': os.chmod(dest,row['mode'] & 0o7777)
        try: os.chown(dest,row['uid'],row['gid'],follow_symlinks=False)
        except PermissionError: raise AssertionError('fsck extraction did not preserve indexed ownership')
    for row in sorted(rows,key=lambda r:len(r['path']),reverse=True):
        if row['kind']=='dir':
            dest=actual if row['path']=='/' else actual/row['path'].lstrip('/')
            os.chmod(dest,row['mode'] & 0o7777); os.chown(dest,row['uid'],row['gid'])
            relative=row['path'].lstrip('/')
            for layer_root in extracted:
                source=layer_root/relative
                if source.is_dir():
                    set_extracted_xattrs(source,dest); break
    compare_tree(expected,actual,rows)
    return actual

with tempfile.TemporaryDirectory(prefix='matonos-erofs-layers-test-') as td:
    t=pathlib.Path(td); repo=t/'repo'; src=t/'src'
    (src/'usr/bin').mkdir(parents=True); (src/'usr/share/doc').mkdir(parents=True)
    (src/'usr/bin/tool').write_bytes(b'unchanged payload'*16)
    (src/'usr/bin/tool').chmod(0o755)
    (src/'usr/share/doc/change').write_bytes(b'old payload')
    (src/'usr/share/doc/delete').write_bytes(b'delete me')
    (src/'usr/share/doc/file-link').write_bytes(b'old link file')
    (src/'usr/share/doc/remove-link').symlink_to('../bin/tool')
    (src/'usr/share/doc/deleted-dir').mkdir(); (src/'usr/share/doc/deleted-dir/child').write_text('gone')
    (src/'usr/share/doc/link').symlink_to('../bin/tool')
    try:
        os.setxattr(src/'usr/bin/tool','user.matonos-test',b'file-xattr')
        os.setxattr(src/'usr/share/doc','user.matonos-test',b'dir-xattr')
    except OSError: pass
    run('ostree','init','--repo='+str(repo),'--mode=bare-user')
    def commit(msg):
        run('ostree','commit','--repo='+str(repo),'-b','test','--tree=dir='+str(src),'-m',msg)
        return subprocess.check_output(['ostree','rev-parse','--repo='+str(repo),'test'],text=True).strip()
    a=commit('A'); first=t/'A'
    run(sys.executable,str(ROOT/'erofs_layers.py'),'--repo',str(repo),'--commit',a,'--out',str(first),'--mkfs',MKFS)
    layer_a,rows_a=read_index(first/'layers.index')
    assert '/usr/bin/tool' in {r['path'] for r in rows_a}
    old_hashes={x['sha256'] for x in layer_a}
    expected_a=t/'expected-A'; run('ostree','checkout','--force-copy','--repo='+str(repo),a,str(expected_a))
    reconstruct(first,(layer_a,rows_a),expected_a,t/'extract-A')
    (src/'usr/share/doc/change').write_bytes(b'new payload')
    (src/'usr/share/doc/new').write_bytes(b'added')
    (src/'usr/share/doc/delete').unlink()
    (src/'usr/share/doc/deleted-dir/child').unlink(); (src/'usr/share/doc/deleted-dir').rmdir()
    (src/'usr/share/doc/file-link').unlink(); (src/'usr/share/doc/file-link').symlink_to('../bin/tool')
    (src/'usr/share/doc/link').unlink(); (src/'usr/share/doc/link').symlink_to('../bin/new-target')
    (src/'usr/share/doc/remove-link').unlink()
    (src/'usr/share/doc/added-link').symlink_to('../bin/tool')
    b=commit('B'); second=t/'B'
    run(sys.executable,str(ROOT/'erofs_layers.py'),'--repo',str(repo),'--commit',b,'--out',str(second),
        '--mkfs',MKFS,'--old-index',str(first/'layers.index'),'--old-layers',str(first))
    layer_b,rows_b=read_index(second/'layers.index'); paths={r['path']:r for r in rows_b}
    assert '/usr/share/doc/delete' not in paths and '/usr/share/doc/deleted-dir/child' not in paths
    assert '/usr/share/doc/remove-link' not in paths
    assert paths['/usr/share/doc/file-link']['kind']=='symlink'
    assert paths['/usr/share/doc/link']['target']=='../bin/new-target'
    assert paths['/usr/share/doc/added-link']['target']=='../bin/tool'
    assert all(paths[p]['layer']!=0xffffffff for p in ('/usr/share/doc/file-link','/usr/share/doc/link','/usr/share/doc/added-link'))
    assert paths['/usr/share/doc/change']['kind']=='file' and paths['/usr/share/doc/new']['kind']=='file'
    assert old_hashes.intersection(x['sha256'] for x in layer_b)
    newly_written=[x for x in layer_b if x['sha256'] not in old_hashes]
    assert len(newly_written)==1, newly_written
    for x in layer_b:
        image=second/('pack-'+x['sha256']+'.erofs')
        assert image.is_file() and image.stat().st_size==x['image_size'] and sha(image)==x['sha256']
    expected_b=t/'expected-B'; run('ostree','checkout','--force-copy','--repo='+str(repo),b,str(expected_b))
    reconstruct(second,(layer_b,rows_b),expected_b,t/'extract-B')
    reader=t/'layers-reader-test'
    subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-I'+str(ROOT),str(ROOT/'reader.c'),
                    str(ROOT/'tests/layers_reader_test.c'),'-o',str(reader)],check=True)
    subprocess.run([str(reader),str(second/'layers.index')],check=True)
    print(f"EROFS layer A→B passed: A={len(layer_a)} layers/{sum(x['image_size'] for x in layer_a)} bytes, "
          f"B={len(layer_b)} layers/{sum(x['image_size'] for x in layer_b)} bytes, "
          f"new={newly_written[0]['image_size']} bytes; mkfs tool={MKFS}")
