#!/usr/bin/env python3
"""Stock ostree CLI + Java bare-user reader versus libostree/pack.py."""
import os, pathlib, subprocess, sys, tempfile, types
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]))
import pack
ROOT=pathlib.Path(__file__).resolve().parents[3]
def run(*a): return subprocess.check_output(a,text=True).strip()
with tempfile.TemporaryDirectory() as tmp:
    base=pathlib.Path(tmp);src=base/'src';src.mkdir();repo=base/'repo'
    (src/'dir').mkdir();(src/'dir/tool').write_bytes(b'payload'*2000);(src/'dir/tool').chmod(0o755)
    (src/'link').symlink_to('dir/tool');(src/'empty').write_bytes(b'')
    for name in ('line\nbreak',"quotes'\\\"",'\ue000','\U00010000'):(src/name).write_bytes(b'same')
    os.setxattr(src/'dir/tool','user.binary',bytes(range(256)))
    os.setxattr(src/'dir/tool','user.text',b"quote'\\\"\n\t")
    os.setxattr(src/'empty','user.long',b'a'*3000)
    os.setxattr(src/'dir','user.directory',b'kept in dirmeta')
    run('ostree','init','--repo='+str(repo),'--mode=bare-user')
    commit=run('ostree','commit','--repo='+str(repo),'-b','test','--tree=dir='+str(src),'-m','test')
    harness=base/'SourceHarness.java'
    harness.write_text('''import java.io.*;import java.nio.charset.StandardCharsets;import java.util.*;
import org.matonos.linuxhost.stubgen.*;
public class SourceHarness {
 public static void main(String[] a)throws Exception {
  OstreePacks.Command cli=(String... args)->{
   List<String> cmd=new ArrayList<>(Arrays.asList("ostree","--repo="+a[0]));cmd.addAll(Arrays.asList(args));
   ProcessBuilder pb=new ProcessBuilder(cmd);pb.environment().put("LC_ALL","C");pb.redirectError(ProcessBuilder.Redirect.INHERIT);
   Process p=pb.start();String out=new String(p.getInputStream().readAllBytes(),StandardCharsets.UTF_8);
   if(p.waitFor()!=0)throw new IOException("ostree failed");return out;
  };
  String rev=a[1];if(a.length>3)rev=OstreePacks.commitDeployed(new File(a[3]),cli);
  OstreePacks.read(new File(a[0]),rev,cli).write(new File(a[2]));System.out.println(rev);
 }
}''')
    java=ROOT/'linux/stubgen/src/org/matonos/linuxhost/stubgen'
    run('javac','-d',str(base),str(java/'ObjectPacks.java'),str(java/'PackLaunch.java'),str(java/'OstreePacks.java'),str(harness))
    def check(label,commit,extra=None):
        jout=base/(label+'-java');pyout=base/(label+'-py')
        args=['java','-cp',str(base),'SourceHarness',str(repo),commit,str(jout)]
        if extra:args.append(str(extra))
        packed=run(*args)
        pack.pack(types.SimpleNamespace(repo=str(repo),commit=packed,out=str(pyout),pack_size=64*1024*1024))
        expected={p.name:p.read_bytes() for p in pyout.iterdir()};actual={p.name:p.read_bytes() for p in jout.iterdir()}
        assert actual==expected,(label,[(k,len(v),len(actual.get(k,b''))) for k,v in expected.items() if actual.get(k)!=v])
        return packed,jout
    check('original',commit)
    # Force four-byte GVariant framing in a real >64KiB dirtree.
    wide=base/'wide';wide.mkdir()
    for n in range(280):(wide/(str(n).zfill(4)+'x'*235)).write_bytes(b'shared')
    wide_commit=run('ostree','commit','--repo='+str(repo),'-b','wide','--tree=dir='+str(wide),'-m','wide')
    check('wide',wide_commit)
    # Real phase-C staging is bare-user-only, including real symlink objects.
    only=base/'only';only.mkdir();(only/'file').write_bytes(b'ordinary');(only/'link').symlink_to('file')
    only_repo=base/'only-repo';run('ostree','init','--repo='+str(only_repo),'--mode=bare-user-only')
    only_commit=run('ostree','commit','--repo='+str(only_repo),'-b','test','--tree=dir='+str(only),'-m','only')
    original_repo=repo;repo=only_repo
    check('bare-user-only',only_commit)
    repo=original_repo
    # Deployment files carry the raw extra downloads the stub applies later,
    # not upstream commit files.
    (src/'extra').mkdir();(src/'extra/downloaded').write_bytes(b'raw extra download')
    new,out=check('extra',commit,src)
    assert new!=commit and any(r['path']=='/extra/downloaded' for r in pack.read_index(out/'objects.index'))
    assert run('ostree','rev-parse','--repo='+str(repo),'matonos/private/deployed-files')==new
    print('PASS stock CLI enumeration, binary metadata, xattrs, unusual filenames, byte parity and local extra commit')
