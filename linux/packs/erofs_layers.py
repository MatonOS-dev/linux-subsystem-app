#!/usr/bin/env python3
"""Build complete EROFS pack layers and a merged-view MTPKIDX2 index."""
import argparse
import base64
import hashlib
import json
import os
import pathlib
import shutil
import stat
import struct
import subprocess
import tarfile
import tempfile
import urllib.parse

from ostree_source import walk_commit, open_repo
from gi.repository import OSTree
from gi.repository import GLib

TARGET = 64 * 1024 * 1024
MAGIC = b'MTPKIDX2'


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def string(out, value):
    b = value.encode('utf-8')
    out.write(struct.pack('<I', len(b))); out.write(b)


def save_index(path, layers, records):
    with open(path, 'wb') as f:
        f.write(MAGIC + struct.pack('<I', len(layers)))
        for layer in layers:
            string(f, layer['split']); string(f, layer['entry'])
            f.write(struct.pack('<Q', layer['image_size']))
            f.write(bytes.fromhex(layer['sha256']))
        f.write(struct.pack('<I', len(records)))
        layer_ids = {x['sha256']: i for i, x in enumerate(layers)}
        for r in sorted(records, key=lambda x: x['path'].encode('utf-8')):
            for k in ('path', 'checksum', 'kind', 'target'):
                string(f, r[k])
            xb = bytes.fromhex(r.get('xattrs', ''))
            f.write(struct.pack('<I', len(xb))); f.write(xb)
            f.write(struct.pack('<IQQIIII', layer_ids.get(r.get('layer_sha256'), 0xffffffff),
                                r.get('size', 0), r.get('mtime', 0), r['uid'], r['gid'], r['mode'], 0))


def read_index(path):
    b = memoryview(pathlib.Path(path).read_bytes()); p = 0
    def take(n):
        nonlocal p
        if p + n > len(b): raise ValueError('truncated layers.index')
        v = b[p:p+n]; p += n; return v
    def text(): return bytes(take(struct.unpack('<I', take(4))[0])).decode('utf-8')
    if bytes(take(8)) != MAGIC: raise ValueError('bad layers.index magic')
    nl = struct.unpack('<I', take(4))[0]; layers = []
    for _ in range(nl):
        split, entry = text(), text(); size = struct.unpack('<Q', take(8))[0]
        layers.append({'split':split, 'entry':entry, 'image_size':size, 'sha256':bytes(take(32)).hex()})
    nr = struct.unpack('<I', take(4))[0]; records = []
    for _ in range(nr):
        r = {k:text() for k in ('path','checksum','kind','target')}
        r['xattrs'] = bytes(take(struct.unpack('<I', take(4))[0])).hex()
        layer, r['size'], r['mtime'], r['uid'], r['gid'], r['mode'], reserved = struct.unpack('<IQQIIII', take(36))
        if reserved: raise ValueError('nonzero reserved field')
        r['layer'] = layer
        records.append(r)
    if p != len(b): raise ValueError('trailing layers.index bytes')
    return layers, records


def commit_epoch(repo_path, commit):
    repo = open_repo(repo_path)
    ok, variant = repo.load_variant(OSTree.ObjectType.COMMIT, commit)
    if not ok: raise ValueError(f'commit not found: {commit}')
    # The timestamp field in OSTree's commit tuple is big-endian u32 seconds;
    # GI exposes its eight-byte variant storage as a host-order u64.
    raw = int(variant.unpack()[5]).to_bytes(8, 'little')
    return int.from_bytes(raw, 'big')


def tar_info(tar, rec, path, epoch):
    info = tarfile.TarInfo('.' if rec['path'] == '/' else './' + rec['path'].lstrip('/'))
    info.uid, info.gid, info.mode = rec['uid'], rec['gid'], stat.S_IMODE(rec['mode'])
    info.mtime = rec.get('mtime') or epoch
    info.uname = info.gname = ''
    raw=bytes.fromhex(rec.get('xattrs',''))
    if raw:
        attrs=GLib.Variant.new_from_bytes(GLib.VariantType.new('a(ayay)'),GLib.Bytes.new(raw),False)
        for key,value in attrs.unpack():
            key=bytes(key).rstrip(b'\0').decode('utf-8')
            info.pax_headers['LIBARCHIVE.xattr.'+urllib.parse.quote(key,safe='')]=base64.b64encode(bytes(value)).decode('ascii')
    if rec['kind'] == 'dir': info.type = tarfile.DIRTYPE; tar.addfile(info)
    elif rec['kind'] == 'symlink':
        info.type = tarfile.SYMTYPE; info.linkname = rec['target']; tar.addfile(info)
    elif rec['kind'] == 'file':
        info.size = rec['size']
        with open(path, 'rb') as src: tar.addfile(info, src)
    else: raise ValueError(f"unsupported kind {rec['kind']}")


def make_image(mkfs, image, records, root, epoch):
    with tempfile.NamedTemporaryFile(suffix='.tar', dir=image.parent, delete=False) as f: tarname = pathlib.Path(f.name)
    try:
        with tarfile.open(tarname, 'w', format=tarfile.PAX_FORMAT) as tar:
            for rec in sorted(records, key=lambda x:x['path'].encode('utf-8')):
                p = root if rec['path'] == '/' else root / rec['path'].lstrip('/')
                tar_info(tar, rec, p, epoch)
        image.unlink(missing_ok=True)
        raw = hashlib.sha256(tarname.read_bytes()).hexdigest()[:32]
        raw = raw[:12] + '5' + raw[13:16] + '8' + raw[17:]
        uuid = f'{raw[:8]}-{raw[8:12]}-{raw[12:16]}-{raw[16:20]}-{raw[20:]}'
        subprocess.run([mkfs, '-zlz4hc,12', '--tar=f', '--mkfs-time', '-T', str(epoch),
                        '--workers=4', '-U', uuid, str(image), str(tarname)], check=True)
    finally: tarname.unlink(missing_ok=True)


def batches(records):
    groups = {}
    for r in records:
        if r['kind'] in ('file', 'symlink'): groups.setdefault(str(pathlib.PurePosixPath(r['path']).parent), []).append(r)
    result, batch, size = [], [], 0
    for parent in sorted(groups):
        for r in sorted(groups[parent], key=lambda x:x['path'].encode()):
            if batch and size + r.get('size', 0) > TARGET: result.append(batch); batch=[]; size=0
            batch.append(r); size += r.get('size', 0)
    if batch: result.append(batch)
    return result


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--repo',required=True); ap.add_argument('--commit',required=True)
    ap.add_argument('--out',required=True,type=pathlib.Path); ap.add_argument('--mkfs',default='mkfs.erofs')
    ap.add_argument('--old-index',type=pathlib.Path); ap.add_argument('--old-layers',type=pathlib.Path)
    ap.add_argument('--dead-ratio',type=float,default=.5)
    a=ap.parse_args()
    if not 0 <= a.dead_ratio <= 1: ap.error('--dead-ratio must be between 0 and 1')
    if (a.old_index is None) != (a.old_layers is None): ap.error('--old-index and --old-layers must be supplied together')
    a.out.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='matonos-erofs-layers-') as td:
        root=pathlib.Path(td)/'tree'
        records,_=walk_commit(a.repo,a.commit,pathlib.Path(td)/'payloads')
        epoch=commit_epoch(a.repo,a.commit)
        # Materialize the complete metadata tree; tar records use the canonical
        # OSTree ownership and mode fields from walk_commit, not host checkout uid.
        subprocess.run(['ostree',f'--repo={a.repo}','checkout','--user-mode',a.commit,str(root)],check=True)
        old_layers, old_records=(read_index(a.old_index) if a.old_index else ([],[]))
        old_layer_by_id={i:x for i,x in enumerate(old_layers)}
        old_by_path={r['path']:r for r in old_records}
        current_paths={r['path'] for r in records}
        for r in records:
            p=root if r['path']=='/' else root/r['path'].lstrip('/')
            r['size']=p.stat().st_size if r['kind']=='file' else 0
        current_files={r['path']:r for r in records if r['kind']=='file'}
        layer_members={}
        for old in old_records:
            if old['kind'] in ('file', 'symlink') and old['layer'] != 0xffffffff:
                layer_members.setdefault(old['layer'],[]).append(old)
        rewrite_layers=set()
        if a.old_index:
            for layer_id,members in layer_members.items():
                total=sum(r['size'] for r in members); live=0
                for old in members:
                    now=current_files.get(old['path'])
                    if old['kind']=='file' and now and now['checksum']==old['checksum'] and now['mode']==old['mode'] and now['uid']==old['uid'] and now['gid']==old['gid']:
                        live+=old['size']
                dead=max(0,total-live)
                if dead and total and dead/total >= a.dead_ratio: rewrite_layers.add(layer_id)
        reused_by_sha={}; changed=[]
        layer_entries=[r for r in records if r['kind'] in ('file', 'symlink')]
        for r in layer_entries:
            old=old_by_path.get(r['path'])
            if old and old['layer'] != 0xffffffff and old['kind']==r['kind'] and old['checksum']==r['checksum'] and old['target']==r['target'] and old['mode']==r['mode'] and old['uid']==r['uid'] and old['gid']==r['gid'] and old.get('xattrs','')==r.get('xattrs',''):
                layer=old_layer_by_id[old['layer']]; reused_by_sha[layer['sha256']]=layer
                if old['layer'] in rewrite_layers: changed.append(r)
                else: r['layer_sha256']=layer['sha256']
            else: changed.append(r)
        for layer_id in rewrite_layers:
            layer=old_layer_by_id[layer_id];reused_by_sha.pop(layer['sha256'],None)
        new_batches=batches(records) if not a.old_index else ([changed] if changed else [])
        layers=[]; emitted={}
        for old in reused_by_sha.values():
            src=a.old_layers / ('pack-'+old['sha256']+'.erofs')
            if not src.is_file() or digest(src)!=old['sha256']: raise ValueError(f'missing/corrupt prior layer: {src}')
            dst=a.out/src.name
            if src.resolve()!=dst.resolve(): shutil.copyfile(src,dst)
            layers.append(dict(old)); emitted[old['sha256']]=old
        written=0
        for batch in new_batches:
            if not batch: continue
            image=a.out/'.pack.tmp.erofs'
            file_paths={r['path'] for r in batch}
            dirs=[r for r in records if r['kind']=='dir' and any(
                p.startswith(r['path'].rstrip('/') + '/') for p in file_paths)]
            # Complete directory metadata is repeated in each pack. This keeps
            # every EROFS image independently traversable while layers.index
            # remains authoritative for the merged view.
            make_image(a.mkfs,image,dirs+batch,root,epoch)
            sha=digest(image); name='pack-'+sha+'.erofs'; dst=a.out/name
            if dst.exists(): image.unlink()
            else: image.replace(dst); written+=1
            layer={'split':'pack_'+sha,'entry':'assets/matonos/pack.erofs','image_size':dst.stat().st_size,'sha256':sha}
            layers.append(layer); emitted[sha]=layer
            for r in batch:r['layer_sha256']=sha
        # The index is a merged path table. Deleted entries simply do not occur.
        for r in records:
            r.setdefault('size',0)
            if r['kind'] in ('file', 'symlink'): r.setdefault('layer_sha256',None)
            if not r.get('mtime'): r['mtime']=epoch
        ordered=sorted(layers,key=lambda x:x['sha256']); save_index(a.out/'layers.index',ordered,records)
        print(json.dumps({'layers':len(ordered),'index_bytes':(a.out/'layers.index').stat().st_size,
                          'pack_bytes':sum(x['image_size'] for x in ordered),'new_packs':written,
                          'deleted':len(set(old_by_path)-current_paths)},sort_keys=True))

if __name__=='__main__': main()
