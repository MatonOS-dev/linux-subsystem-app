#!/usr/bin/env python3
"""Deterministic OSTree object pack writer (Python 3, host side)."""
import argparse, hashlib, json, pathlib, re, shutil, struct, tempfile, zipfile
import ostree_source

MAGIC = b'MTPKIDX1'
PAGE = 4096


def align_extra(name, header_offset):
    # ZIP extra fields: private ID 0x4d54, then zero bytes for local alignment.
    pad = (-(header_offset + 30 + len(name.encode()) + 4)) % PAGE
    return struct.pack('<HH', 0x4d54, pad) + bytes(pad)

def zip_pack(path, objects):
    with zipfile.ZipFile(path, 'w', allowZip64=True) as z:
        for obj in sorted(objects, key=lambda x: x['checksum']):
            name = obj['checksum']
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            info.create_system = 3
            info.external_attr = (obj['mode'] & 0xffff) << 16
            info.extra = align_extra(name, z.fp.tell())
            z.writestr(info, pathlib.Path(obj['source']).read_bytes())
            obj['pack'] = path.name
            obj['offset'] = info.header_offset + 30 + len(name.encode()) + len(info.extra)
            obj['length'] = pathlib.Path(obj['source']).stat().st_size
            if obj['offset'] % PAGE:
                raise ValueError('internal error: unaligned payload')

def save_index(path, records):
    with open(path, 'wb') as f:
        f.write(MAGIC + struct.pack('<I', len(records)))
        for r in sorted(records, key=lambda x: x['path'].encode()):
            for k in ('path', 'checksum', 'pack', 'kind', 'target'):
                b = r[k].encode(); f.write(struct.pack('<I', len(b))); f.write(b)
            xb = bytes.fromhex(r.get('xattrs', ''))
            f.write(struct.pack('<I', len(xb))); f.write(xb)
            f.write(struct.pack('<QQIII', r['offset'], r['length'], r['uid'], r['gid'], r['mode']))

def files_at(repo, commit, payload_dir):
    records, objects = ostree_source.walk_commit(repo, commit, payload_dir)
    # The base metadata blob preserves the canonical commit, dirtree and
    # dirmeta encodings so a reader can walk/verify the original OSTree tree.
    return records, objects

def save_metadata(path, objects):
    with open(path, 'wb') as f:
        f.write(b'MTPKOBJ1' + struct.pack('<I', len(objects)))
        for checksum, raw in sorted(objects.items()):
            f.write(bytes.fromhex(checksum) + struct.pack('<Q', len(raw)) + raw)


def make_packs(records, out, pack_size):
    # Directory locality; pack size is a target, never a hard limit.
    groups={}
    unique={}
    for r in records: unique.setdefault(r['checksum'], r)
    for r in unique.values(): groups.setdefault(str(pathlib.PurePosixPath(r['path']).parent), []).append(r)
    batches=[]; cur=[]; size=0
    for directory in sorted(groups):
        for r in sorted(groups[directory], key=lambda x:x['path']):
            n=pathlib.Path(r['source']).stat().st_size
            if cur and size+n > pack_size: batches.append(cur); cur=[]; size=0
            cur.append(r); size += n
    if cur: batches.append(cur)
    for batch in batches:
        tmp=out / '.pack.tmp.zip'; zip_pack(tmp, batch)
        digest=hashlib.sha256(tmp.read_bytes()).hexdigest(); dest=out / ('pack-'+digest+'.zip')
        tmp.replace(dest)
        for r in batch:
            r['pack']=dest.name
            for alias in records:
                if alias['checksum']==r['checksum']:
                    alias.update(pack=r['pack'],offset=r['offset'],length=r['length'])

def pack(args):
    out=pathlib.Path(args.out); out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as d:
        records, objects=files_at(args.repo,args.commit,pathlib.Path(d))
        files=[r for r in records if r['kind']=='file']; make_packs(files,out,args.pack_size)
        for r in records:
            if r['kind']!='file': r.update(pack='',offset=0,length=0)
        save_metadata(out/'objects.ostree',objects)
        save_index(out/'objects.index',records)
        print(json.dumps({'objects':len(records),'metadata_objects':len(objects),'bytes_written':sum(r['length'] for r in files)},sort_keys=True))

def read_index(path):
    # Kept in lockstep with reader.c; strict truncation and integer checks.
    b=memoryview(pathlib.Path(path).read_bytes()); p=0
    def take(n):
        nonlocal p
        if n<0 or p+n>len(b): raise ValueError('truncated index')
        x=b[p:p+n]; p+=n; return x
    if bytes(take(8))!=MAGIC: raise ValueError('bad index magic')
    count=struct.unpack('<I',take(4))[0]; out=[]
    for _ in range(count):
        r={}
        for k in ('path','checksum','pack','kind','target'):
            n=struct.unpack('<I',take(4))[0]; r[k]=bytes(take(n)).decode()
        n=struct.unpack('<I',take(4))[0]; r['xattrs']=bytes(take(n)).hex()
        r['offset'],r['length'],r['uid'],r['gid'],r['mode']=struct.unpack('<QQIII',take(28))
        out.append(r)
    if p!=len(b): raise ValueError('trailing index data')
    return out

def update(args):
    out=pathlib.Path(args.out); out.mkdir(parents=True,exist_ok=True)
    old=read_index(args.old_index)
    with tempfile.TemporaryDirectory() as d:
        fresh, objects=files_at(args.repo,args.commit,pathlib.Path(d))
        # A content checksum is content-addressed; unchanged objects keep their old pack.
        old_checksum={x['checksum']:x for x in old}
        records=[]; new=[]
        for r in fresh:
            prior=old_checksum.get(r['checksum']) if r['kind']=='file' else None
            if prior:
                r.update(pack=prior['pack'],offset=prior['offset'],length=prior['length'])
            elif r['kind']=='file': new.append(r)
            else: r.update(pack='',offset=0,length=0)
            records.append(r)
        if new:
            # Update's newly introduced objects are exactly one pack.
            unique_new=list({r['checksum']:r for r in new}.values())
            temp=out/'.new.zip'; zip_pack(temp,unique_new)
            dest=out/('pack-'+hashlib.sha256(temp.read_bytes()).hexdigest()+'.zip'); temp.replace(dest)
            for r in unique_new: r['pack']=dest.name
            for r in new:
                source=next(x for x in unique_new if x['checksum']==r['checksum'])
                r.update(pack=source['pack'],offset=source['offset'],length=source['length'])
        # Measure cumulative dead payload in carriers the newest index retains.
        by_pack={}
        for x in old: by_pack.setdefault(x['pack'],[]).append(x)
        live={x['checksum'] for x in records}
        unique_new=list({x['checksum']:x for x in new}.values())
        written=sum(x['length'] for x in unique_new); changed=written
        retained={}; total=written; dead=0
        for packname, members in by_pack.items():
            members=list({x['checksum']:x for x in members}.values())
            live_members=[x for x in members if x['kind']=='file' and x['checksum'] in live]
            if not live_members: continue
            retained[packname]=live_members
            needed={x['checksum'] for x in live_members}
            with zipfile.ZipFile(pathlib.Path(args.old_packs)/packname) as z:
                entries=[x for x in z.infolist() if re.fullmatch('[0-9a-f]{64}',x.filename)]
                total+=sum(x.file_size for x in entries)
                dead+=sum(x.file_size for x in entries if x.filename not in needed)
        repack=bool(total and dead and dead/total >= args.dead_ratio)
        for packname, live_members in retained.items():
            if repack:
                src=pathlib.Path(args.old_packs)/packname
                with zipfile.ZipFile(src) as z:
                    for r in records:
                        if r['checksum'] in {x['checksum'] for x in live_members}:
                            extracted=pathlib.Path(d)/r['checksum']; extracted.write_bytes(z.read(r['checksum'])); r['source']=str(extracted)
                rewrite=[r for r in records if r.get('pack')==packname]
                unique_rewrite=list({r['checksum']:r for r in rewrite}.values())
                tmp=out/'.rewrite.zip'; zip_pack(tmp,unique_rewrite)
                dest=out/('pack-'+hashlib.sha256(tmp.read_bytes()).hexdigest()+'.zip'); tmp.replace(dest)
                size=sum(r['length'] for r in unique_rewrite); written+=size
                for r in unique_rewrite:r['pack']=dest.name
                for r in rewrite:
                    source=next(x for x in unique_rewrite if x['checksum']==r['checksum'])
                    r.update(pack=dest.name,offset=source['offset'],length=source['length'])
        # Make update output self-contained while retaining immutable old packs.
        for packname in {r['pack'] for r in records if r['pack']}:
            target=out/packname
            if not target.exists(): shutil.copy2(pathlib.Path(args.old_packs)/packname,target)
        save_metadata(out/'objects.ostree',objects)
        save_index(out/'objects.index',records)
        amp=(written/changed if changed else 0)
        print(json.dumps({'bytes_written':written,'bytes_changed':changed,'write_amplification':amp,'new_objects':len(unique_new)},sort_keys=True))

def main():
    ap=argparse.ArgumentParser(); sub=ap.add_subparsers(dest='cmd',required=True)
    p=sub.add_parser('pack'); p.add_argument('--repo',required=True);p.add_argument('--commit',required=True);p.add_argument('--out',required=True);p.add_argument('--pack-size',type=int,default=64*1024*1024);p.set_defaults(fn=pack)
    u=sub.add_parser('update');u.add_argument('--repo',required=True);u.add_argument('--commit',required=True);u.add_argument('--old-index',required=True);u.add_argument('--old-packs',required=True);u.add_argument('--out',required=True);u.add_argument('--dead-ratio',type=float,default=.5);u.add_argument('--pack-size',type=int,default=64*1024*1024);u.set_defaults(fn=update)
    a=ap.parse_args();
    if a.pack_size<1 or (a.cmd=='update' and not 0<=a.dead_ratio<=1): ap.error('invalid sizing policy')
    a.fn(a)
if __name__=='__main__': main()
