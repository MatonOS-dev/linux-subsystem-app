"""No strace: fallback walk diagnostics must remain absent, including replay."""
import pathlib, subprocess, tempfile, os, signal, time
base=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='incremental-',dir=base/'build') as tmp:
    root=pathlib.Path(tmp).resolve();(root/'src').mkdir(mode=0o700);(root/'dst').mkdir(mode=0o700);(root/'.v3-inodes').mkdir(mode=0o700)
    for i in range(20000): (root/'src'/str(i)).touch()
    for i in range(5000): (root/'dst'/str(i)).touch()
    args=[str(base/'build/link-deny'),str(base/'build/links-test-loader'),'--android-filter',str(base/'build/links-incremental-test'),str(root)]
    result=subprocess.run(args+['scale'],capture_output=True,text=True,timeout=120)
    print(result.stdout,end='');assert result.returncode==0,result.stderr
    assert 'links tree walk' not in result.stderr,result.stderr
    store=root/'.v3-inodes'
    untouched=(root/'src/1').resolve().with_suffix('.count')
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

    backing=(root/'src/3').resolve()
    result=subprocess.run(args+['mutate'],capture_output=True,text=True,timeout=15)
    print(result.stdout,end='');assert result.returncode==0,result.stderr
    assert 'links tree walk' not in result.stderr,result.stderr
    assert not backing.exists() and not pathlib.Path(str(backing)+'.count').exists()

with tempfile.TemporaryDirectory(prefix='incremental-corrupt-',dir=base/'build') as tmp:
    root=pathlib.Path(tmp).resolve();store=root/'.v3-inodes';store.mkdir(mode=0o700);(root/'src').mkdir(mode=0o700)
    for i in range(2):
        obj=store/('i-'+str(i));obj.touch();(root/'src'/str(i)).symlink_to(obj)
        pathlib.Path(str(obj)+'.count').write_bytes(b'x' if i==0 else (1).to_bytes(8,'little'))
    args=[str(base/'build/link-deny'),str(base/'build/links-test-loader'),'--android-filter',str(base/'build/links-incremental-test'),str(root),'fallback']
    result=subprocess.run(args,capture_output=True,text=True,timeout=15)
    print(result.stdout,end='');assert result.returncode==0,result.stderr
    assert result.stderr.count('links tree walk')==4,result.stderr # two objects, root + src each
