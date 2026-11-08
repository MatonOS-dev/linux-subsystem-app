"""No strace: fallback walk diagnostics must remain absent, including replay."""
import pathlib, subprocess, tempfile, os, signal, time
base=pathlib.Path(__file__).resolve().parents[1]
out=pathlib.Path(os.environ.get('COMPAT_LINKS_TEST_OUT',base/'build'))
def backing(root,path):
    st=path.stat();store=root/'.v3-inodes'
    return store/(store/f'p-{st.st_ino:016x}').read_bytes().rstrip(b'\0').decode()

with tempfile.TemporaryDirectory(prefix='incremental-',dir=out) as tmp:
    root=pathlib.Path(tmp).resolve();(root/'.v3-store-root').touch();(root/'src').mkdir(mode=0o700);(root/'dst').mkdir(mode=0o700);(root/'.v3-inodes').mkdir(mode=0o700)
    for i in range(20000): (root/'src'/str(i)).touch()
    for i in range(5000): (root/'dst'/str(i)).touch()
    args=[str(out/'link-deny'),str(out/'links-test-loader'),'--android-filter',str(out/'links-incremental-test'),str(root)]
    result=subprocess.run(args+['scale'],capture_output=True,text=True,timeout=120)
    print(result.stdout,end='');assert result.returncode==0,result.stderr
    assert 'links tree walk' not in result.stderr,result.stderr
    store=root/'.v3-inodes'
    untouched=pathlib.Path(str(backing(root,root/'src/1'))+'.count')
    before=(untouched.read_bytes(),untouched.stat().st_mtime_ns)
    (store/'stop-intent').touch()
    child=subprocess.Popen(args+['crash'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    deadline=time.monotonic()+15
    while time.monotonic()<deadline:
        pid,status=os.waitpid(child.pid,os.WUNTRACED|os.WNOHANG)
        if pid:
            assert os.WIFSTOPPED(status),status
            break
        time.sleep(.01)
    else:
        child.kill();child.wait();raise AssertionError('intent hook did not stop')
    child.kill();child.communicate();(store/'stop-intent').unlink()
    assert (store/'intent').exists()
    result=subprocess.run(args+['recover'],capture_output=True,text=True,timeout=15)
    print(result.stdout,end='');assert result.returncode==0,result.stderr
    assert 'links tree walk' not in result.stderr,result.stderr
    assert not (store/'intent').exists()
    assert before==(untouched.read_bytes(),untouched.stat().st_mtime_ns)
    print('PASS zero tree walks during 5000 conversions/links, 25000 stats and killed-operation recovery')

    retired=backing(root,root/'src/3')
    result=subprocess.run(args+['mutate'],capture_output=True,text=True,timeout=15)
    print(result.stdout,end='');assert result.returncode==0,result.stderr
    assert 'links tree walk' not in result.stderr,result.stderr
    assert not retired.exists() and not pathlib.Path(str(retired)+'.count').exists()

with tempfile.TemporaryDirectory(prefix='incremental-corrupt-',dir=out) as tmp:
    root=pathlib.Path(tmp).resolve();(root/'.v3-store-root').touch();store=root/'.v3-inodes';store.mkdir(mode=0o700);(root/'src').mkdir(mode=0o700)
    for i in range(2):
        obj=store/('i-'+f'{i:032x}');obj.touch();p=root/'src'/str(i);p.touch();p.chmod(0o1000);st=p.stat()
        (store/f'p-{st.st_ino:016x}').write_bytes(obj.name.encode()+b'\0')
        pathlib.Path(str(obj)+'.count').write_bytes(b'x' if i==0 else (1).to_bytes(8,'little'))
    args=[str(out/'link-deny'),str(out/'links-test-loader'),'--android-filter',str(out/'links-incremental-test'),str(root),'fallback']
    result=subprocess.run(args,capture_output=True,text=True,timeout=15)
    print(result.stdout,end='');assert result.returncode==0,result.stderr
    assert result.stderr.count('links tree walk')==2,result.stderr # one pass: root + src

for dirty in (False,True):
  with tempfile.TemporaryDirectory(prefix='incremental-heal-',dir=out) as tmp:
    root=pathlib.Path(tmp).resolve();(root/'.v3-store-root').touch();store=root/'.v3-inodes';store.mkdir(mode=0o700);(root/'src').mkdir(mode=0o700)
    obj=store/('i-'+'ab'*16);obj.write_bytes(b'data');pathlib.Path(str(obj)+'.count').write_bytes((1).to_bytes(8,'little'))
    p=root/'src'/'0';p.write_bytes(obj.name.encode()+b'\0');p.chmod(0o1000)
    if dirty:(store/'dirty').touch()
    args=[str(out/'link-deny'),str(out/'links-test-loader'),'--android-filter',str(out/'links-incremental-test'),str(root),'heal']
    result=subprocess.run(args,capture_output=True,text=True,timeout=15)
    print(result.stdout,end='');assert result.returncode==0,result.stderr
    assert 'rebuilt from placeholder backup' in result.stderr,result.stderr
    assert (store/f'p-{p.stat().st_ino:016x}').exists() and p.stat().st_mode&0o7777==0o1000

for kind in ('empty','gone'):
  with tempfile.TemporaryDirectory(prefix='incremental-dead-',dir=out) as tmp:
    root=pathlib.Path(tmp).resolve();(root/'.v3-store-root').touch();store=root/'.v3-inodes';store.mkdir(mode=0o700);(root/'src').mkdir(mode=0o700)
    p=root/'src'/'0';p.write_bytes(b'' if kind=='empty' else b'i-'+b'cd'*16+b'\0');p.chmod(0o1000)
    args=[str(out/'link-deny'),str(out/'links-test-loader'),'--android-filter',str(out/'links-incremental-test'),str(root),'dead']
    result=subprocess.run(args,capture_output=True,text=True,timeout=15)
    print(result.stdout,end='');assert result.returncode==0,result.stderr
    assert 'dead placeholder deleted' in result.stderr,result.stderr
