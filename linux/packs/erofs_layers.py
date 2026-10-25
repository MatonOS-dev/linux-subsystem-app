#!/usr/bin/env python3
"""Build deterministic, tar-backed EROFS overlay layers from an OSTree commit."""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import stat
import subprocess
import tarfile
import tempfile

TARGET = 64 * 1024 * 1024


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def add_path(tar, path, arc, uid, gid):
    if isinstance(path, tuple) and path[0] == 'whiteout':
        info = tarfile.TarInfo(arc)
        info.type = tarfile.CHRTYPE; info.devmajor = info.devminor = 0
        info.uid, info.gid, info.mode, info.mtime = uid, gid, 0, 0
        tar.addfile(info)
        return
    st = path.lstat()
    info = tarfile.TarInfo(arc)
    info.uid, info.gid, info.mode, info.mtime = uid, gid, stat.S_IMODE(st.st_mode), 0
    info.uname = info.gname = ''
    try:
        for key in os.listxattr(path, follow_symlinks=False):
            info.pax_headers['SCHILY.xattr.' + key] = os.getxattr(path, key, follow_symlinks=False).decode('latin1')
    except (OSError, AttributeError):
        pass
    if stat.S_ISDIR(st.st_mode):
        info.type = tarfile.DIRTYPE
        tar.addfile(info)
    elif stat.S_ISLNK(st.st_mode):
        info.type = tarfile.SYMTYPE
        info.linkname = os.readlink(path)
        tar.addfile(info)
    elif stat.S_ISREG(st.st_mode):
        info.size = st.st_size
        with open(path, 'rb') as f:
            tar.addfile(info, f)
    elif stat.S_ISCHR(st.st_mode) and st.st_rdev == 0:
        info.type = tarfile.CHRTYPE
        info.devmajor = info.devminor = 0
        tar.addfile(info)
    else:
        raise ValueError(f'unsupported OSTree node: {path}')


def make_image(mkfs, image, nodes, base, uid, gid, epoch):
    image.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(suffix='.tar', dir=image.parent, delete=False) as f:
        tarname = pathlib.Path(f.name)
    try:
        with tarfile.open(tarname, 'w', format=tarfile.PAX_FORMAT) as tar:
            def key(value): return (value[1] if isinstance(value, tuple) else value).encode()
            for rel in sorted(nodes, key=key):
                if isinstance(rel, tuple):
                    deleted = rel[1].lstrip('/')
                    add_path(tar, ('whiteout', deleted), './' + deleted, uid, gid)
                else:
                    path = base / rel.lstrip('/') if rel else base
                    add_path(tar, path, '.' if not rel else './' + rel.lstrip('/'), uid, gid)
        image.unlink(missing_ok=True)
        image_uuid = hashlib.sha256(tarname.read_bytes()).hexdigest()[:32]
        image_uuid = image_uuid[:12] + '5' + image_uuid[13:16] + '8' + image_uuid[17:]
        subprocess.run([mkfs, '-zlz4hc,12', '--tar=f', '--mkfs-time', '-T', str(epoch), '--workers=4',
                        '-U', image_uuid,
                        str(image), str(tarname)], check=True)
    finally:
        tarname.unlink(missing_ok=True)


def all_nodes(root):
    result = ['']
    for base, dirs, files in os.walk(root, topdown=True, followlinks=False):
        dirs.sort(); files.sort()
        relbase = pathlib.Path(base).relative_to(root)
        for name in dirs + files:
            p = pathlib.Path(base) / name
            rel = (relbase / name).as_posix()
            result.append(rel)
            if p.is_symlink():
                dirs[:] = [d for d in dirs if d != name]
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', required=True); p.add_argument('--commit', required=True)
    p.add_argument('--out', required=True, type=pathlib.Path); p.add_argument('--mkfs', default='mkfs.erofs')
    p.add_argument('--uid', type=int, default=0); p.add_argument('--gid', type=int, default=0)
    p.add_argument('--old-layers', type=pathlib.Path)
    a = p.parse_args(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='matonos-erofs-layers-') as td:
        root = pathlib.Path(td) / 'tree'
        subprocess.run(['ostree', f'--repo={a.repo}', 'checkout', '--user-mode', a.commit, str(root)], check=True)
        nodes = all_nodes(root)
        # Match pack.py policy: parent directory groups, lexical traversal, target is not a cap.
        groups = {}
        for rel in nodes:
            path = root / rel if rel else root
            if path.is_file() and not path.is_symlink():
                groups.setdefault(str(pathlib.PurePosixPath('/' + rel).parent), []).append(rel)
        batches, batch, size = [], [], 0
        for directory in sorted(groups):
            for rel in sorted(groups[directory]):
                n = (root / rel).stat().st_size
                if batch and size + n > TARGET: batches.append(batch); batch = []; size = 0
                batch.append(rel); size += n
        if batch: batches.append(batch)
        previous = json.loads(a.old_layers.read_text()) if a.old_layers else None
        old_paths = set()
        old_by_path = {}
        if previous:
            for layer in previous['layers'][1:]:
                for item in layer.get('files', []):
                    old_paths.add(item['path']); old_by_path[item['path']] = (layer, item)
        current = set('/' + n for n in nodes if n)
        removed = old_paths - current
        # Index carries all directory metadata and symlinks; overlay whiteouts hide removed paths.
        index_nodes = [n for n in nodes if not n or (root / n).is_dir() or (root / n).is_symlink()]
        for rel in sorted(removed): index_nodes.append(('whiteout', rel))
        current_files = {}
        for rel in nodes:
            path = root / rel if rel else root
            if path.is_file() and not path.is_symlink():
                st = path.stat()
                current_files['/' + rel] = {'path': '/' + rel, 'object_sha256': digest(path),
                                             'mode': stat.S_IMODE(st.st_mode), 'rel': rel}
        reused, changed = [], []
        for item in current_files.values():
            old = old_by_path.get(item['path'])
            if old and old[1].get('object_sha256') == item['object_sha256'] and old[1].get('mode') == item['mode']:
                reused.append(old[0])
            else: changed.append(item)
        if previous:
            batches = [[item['rel'] for item in sorted(changed, key=lambda x: x['path'].encode())]] if changed else []
        pack_layers = []
        for batch in batches:
            if not batch: continue
            image = out / 'pack.erofs'
            make_image(a.mkfs, image, batch, root, a.uid, a.gid, 0)
            sha = digest(image); name = 'pack-' + sha + '.erofs'
            target = out / name
            if not target.exists(): image.replace(target)
            else: image.unlink()
            pack_layers.append({'split': 'pack_' + sha, 'entry': 'assets/matonos/pack.erofs',
                                'image_size': target.stat().st_size, 'sha256': sha,
                                'files': [current_files['/' + x] for x in batch]})
        # Retained immutable images are copied byte-for-byte into the new layer set.
        if previous:
            used = set()
            for layer in reused:
                if layer['sha256'] in used: continue
                used.add(layer['sha256'])
                source = a.old_layers.parent / layer['filename']
                target = out / source.name
                if source.resolve() != target.resolve(): shutil.copyfile(source, target)
                pack_layers.append(dict(layer))
        index = out / 'index.erofs'
        make_image(a.mkfs, index, index_nodes, root, a.uid, a.gid, 0)
        sha = digest(index)
        layers = [{'split': 'index_' + sha, 'entry': 'assets/matonos/index.erofs',
                   'image_size': index.stat().st_size, 'sha256': sha, 'filename': 'index.erofs',
                   'files': [], 'whiteouts': sorted(removed)}]
        for layer in pack_layers: layer.setdefault('filename', 'pack-' + layer['sha256'] + '.erofs')
        layers += pack_layers
        result = {'format': 'matonos-erofs-layers-v1', 'layers': layers}
        (out / 'layers.json').write_text(json.dumps(result, sort_keys=True, separators=(',', ':')) + '\n')
        print(json.dumps({'layers': len(layers), 'index_bytes': index.stat().st_size,
                          'pack_bytes': sum(x['image_size'] for x in layers[1:]), 'deleted': len(removed)}, sort_keys=True))


if __name__ == '__main__': main()
