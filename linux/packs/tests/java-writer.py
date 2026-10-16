#!/usr/bin/env python3
"""Compare Java writer with reference using real libostree commit records."""
import pathlib, subprocess, tempfile, sys, types, hashlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import pack
ROOT=pathlib.Path(__file__).resolve().parents[3]
JAVA=ROOT/'linux/stubgen/src/org/matonos/linuxhost/stubgen/ObjectPacks.java'
def run(*args): return subprocess.check_output(args,text=True).strip()
with tempfile.TemporaryDirectory() as tmp:
    base=pathlib.Path(tmp); src=base/'src';src.mkdir();repo=base/'repo'
    run('ostree','init','--repo='+str(repo),'--mode=bare-user')
    (src/'dir').mkdir();(src/'dir/tool').write_bytes(b'a'*8192);(src/'dir/tool').chmod(0o755)
    (src/'empty').write_bytes(b'');(src/'alias').write_bytes(b'a'*8192);(src/'link').symlink_to('dir/tool')
    (src/'keep').write_bytes(b'keep'*512)
    (src/'\ue000').write_bytes(b'unicode-a');(src/'\U00010000').write_bytes(b'unicode-b')
    harness=base/'PackHarness.java'
    harness.write_text('''import java.io.*;import java.nio.file.*;import java.util.*;
import org.matonos.linuxhost.stubgen.ObjectPacks;
public class PackHarness {
 static List<ObjectPacks.Entry> read(String name)throws Exception {
  List<ObjectPacks.Entry> out=new ArrayList<>();
  for(String line:Files.readAllLines(Path.of(name))) {
   String[] f=line.split("\\t",-1);ObjectPacks.Entry e=new ObjectPacks.Entry(f[0],f[1],f[2],Integer.parseInt(f[3]));
   e.uid=Integer.parseInt(f[4]);e.gid=Integer.parseInt(f[5]);e.target=f[6];e.source=f[7].isEmpty()?null:new File(f[7]);
   e.xattrs=HexFormat.of().parseHex(f[8]);e.pack=f[9];e.offset=Long.parseLong(f[10]);e.length=Long.parseLong(f[11]);out.add(e);
  }return out;
 }
 public static void main(String[] a)throws Exception {
  List<ObjectPacks.Entry> rows=read(a[0]);File out=new File(a[1]);
  if(a.length==2)ObjectPacks.initial(rows,out,10000);
  else ObjectPacks.update(rows,read(a[2]),new File(a[3]),out,Double.parseDouble(a[4]));
  ObjectPacks.writeIndex(new File(out,"objects.index"),rows);
  Map<String,byte[]> metadata=new HashMap<>();
  for(File f:new File(a[0]+".metadata").listFiles())metadata.put(f.getName(),Files.readAllBytes(f.toPath()));
  ObjectPacks.writeMetadata(new File(out,"objects.ostree"),metadata);
 }
}''')
    run('javac','-d',str(base),str(JAVA),str(JAVA.with_name('PackLaunch.java')),str(harness))
    def table(path, rows):
        path.write_text(''.join('\t'.join(str(r.get(k,'')) for k in ('path','checksum','kind','mode','uid','gid','target','source','xattrs','pack','offset','length'))+'\n' for r in rows))
    def commit(n):
        run('ostree','commit','--repo='+str(repo),'-b','test','--tree=dir='+str(src),'-m',str(n))
        return run('ostree','rev-parse','--repo='+str(repo),'test')
    commits=[commit(1)]
    (src/'dir/tool').write_bytes(b'b'*8192);(src/'alias').unlink();(src/'new').write_bytes(b'new');commits.append(commit(2))
    (src/'keep').unlink();commits.append(commit(3))
    for ratio in (0,.5,.75,1):
        previous=None
        for n,c in enumerate(commits):
            py=base/f'py-{ratio}-{n}';j=base/f'java-{ratio}-{n}';payload=base/f'payload-{ratio}-{n}'
            rows,objects=pack.files_at(str(repo),c,payload)
            for r in rows:r.update(pack='',offset=0,length=0)
            t=base/'rows.tsv';table(t,rows)
            md=base/'rows.tsv.metadata';md.mkdir(exist_ok=True)
            for p in md.iterdir():p.unlink()
            for checksum,raw in objects.items():(md/checksum).write_bytes(raw)
            args=[str(t),str(j)]
            opts=dict(repo=str(repo),commit=c,out=str(py),pack_size=10000,dead_ratio=ratio)
            if previous:
                old=base/'old.tsv';table(old,pack.read_index(previous/'objects.index'))
                args += [str(old),str(previous),str(ratio)]
                pack.update(types.SimpleNamespace(**opts,old_index=str(previous/'objects.index'),old_packs=str(previous)))
            else:pack.pack(types.SimpleNamespace(**opts))
            run('java','-cp',str(base),'PackHarness',*args)
            expected={p.name:p.read_bytes() for p in py.iterdir()}
            actual={p.name:p.read_bytes() for p in j.iterdir()}
            assert expected==actual,(ratio,n,set(expected)^set(actual),[(k,len(v),len(actual.get(k,b''))) for k,v in expected.items() if actual.get(k)!=v])
            previous=py
    print('PASS Java bytes equal pack.py: real commits, initial and two updates, thresholds 0/.5/.75/1')
