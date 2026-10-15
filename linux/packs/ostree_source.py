"""Read canonical OSTree objects from a bare-user repository via libostree."""
import gi

gi.require_version('OSTree', '1.0')
from gi.repository import Gio, GLib, OSTree


def open_repo(path):
    repo = OSTree.Repo.new(Gio.File.new_for_path(path))
    repo.open(None)
    return repo


def _raw(repo, kind, checksum):
    stream, size = repo.load_object_stream(kind, checksum, None)[1:]
    chunks = []
    while True:
        block = stream.read_bytes(1024 * 1024, None).get_data()
        if not block:
            break
        chunks.append(block)
    return b''.join(chunks)


def _checksum(value):
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, GLib.Bytes):
        return value.get_data().hex()
    return bytes(value).hex()


def commit_bytes(repo, commit):
    return _raw(repo, OSTree.ObjectType.COMMIT, commit)


def walk_commit(repo_path, commit, destination):
    """Return file records and exact raw commit/dirtree/dirmeta objects.

    File payload bytes are materialized under destination by canonical file
    checksum; records include all filesystem entries, including directories
    and symlinks. ``objects`` maps canonical metadata checksum to raw bytes.
    """
    repo = open_repo(repo_path)
    ok, commit_v = repo.load_variant(OSTree.ObjectType.COMMIT, commit)
    if not ok:
        raise ValueError(f'commit not found: {commit}')
    fields = commit_v.unpack()
    root_tree, root_meta = _checksum(fields[6]), _checksum(fields[7])
    objects = {commit: commit_bytes(repo, commit)}
    records = []
    destination.mkdir(parents=True, exist_ok=True)

    def metadata(checksum):
        if checksum not in objects:
            objects[checksum] = _raw(repo, OSTree.ObjectType.DIR_META, checksum)
        ok, variant = repo.load_variant(OSTree.ObjectType.DIR_META, checksum)
        if not ok:
            raise ValueError(f'missing dirmeta {checksum}')
        values = variant.unpack()
        swap = lambda value: int.from_bytes(value.to_bytes(4, 'little'), 'big')
        uid, gid, mode, attrs = values
        return swap(mode), swap(uid), swap(gid), attrs

    def visit(tree_sum, meta_sum, prefix):
        mode, uid, gid, _ = metadata(meta_sum)
        if not prefix:
            records.append(dict(path='/', checksum=tree_sum, kind='dir', target='', uid=uid, gid=gid, mode=mode, xattrs=''))
        objects[tree_sum] = _raw(repo, OSTree.ObjectType.DIR_TREE, tree_sum)
        ok, tree = repo.load_variant(OSTree.ObjectType.DIR_TREE, tree_sum)
        if not ok:
            raise ValueError(f'missing dirtree {tree_sum}')
        files, dirs = tree.unpack()
        for name, tree_bytes, meta_bytes in dirs:
            child_tree, child_meta = _checksum(tree_bytes), _checksum(meta_bytes)
            path = f'{prefix}/{name}' if prefix else '/' + name
            child_mode, child_uid, child_gid, _ = metadata(child_meta)
            records.append(dict(path=path, checksum=child_tree, kind='dir', target='',
                                uid=child_uid, gid=child_gid, mode=child_mode, xattrs=''))
            visit(child_tree, child_meta, path)
        for name, checksum in files:
            file_sum = _checksum(checksum)
            loaded, stream, info, xattrs = repo.load_file(file_sum, None)
            if not loaded:
                raise ValueError(f'missing file object {file_sum}')
            mode = info.get_attribute_uint32('unix::mode')
            uid = info.get_attribute_uint32('unix::uid')
            gid = info.get_attribute_uint32('unix::gid')
            chunks = []
            if stream is not None:
                while True:
                    block = stream.read_bytes(1024 * 1024, None).get_data()
                    if not block:
                        break
                    chunks.append(block)
            payload = b''.join(chunks)
            target = info.get_symlink_target() or '' if (mode & 0o170000) == 0o120000 else ''
            source = ''
            if (mode & 0o170000) == 0o100000:
                source_path = destination / file_sum
                source_path.write_bytes(payload)
                source = str(source_path)
            records.append(dict(path=f'{prefix}/{name}' if prefix else '/' + name,
                                checksum=file_sum, kind='symlink' if target else 'file',
                                target=target, source=source, uid=uid, gid=gid, mode=mode,
                                xattrs=xattrs.get_data_as_bytes().get_data().hex() if xattrs else ''))
    visit(root_tree, root_meta, '')
    return records, objects
