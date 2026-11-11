"""RENAME_EXCHANGE conversion checkpoints: neither name ever loses its data."""
import os, pathlib, subprocess, tempfile
base=pathlib.Path(__file__).resolve().parents[1]
out=pathlib.Path(os.environ.get('COMPAT_LINKS_TEST_OUT',base/'build'))

def indexed(store, path, obj):
    path.touch();path.chmod(0o1000);st=path.stat()
    (store/f'p-{st.st_ino:016x}').write_bytes(obj.name.encode()+b'\0')

for phase in ('prepared','staged','exchanged','counted','target-staged'):
    with tempfile.TemporaryDirectory(prefix='conversion-',dir=out) as tmp:
        root=pathlib.Path(tmp).resolve()
        store=root/'.v3-inodes';store.mkdir(mode=0o700)
        source=root/'restored';obj=store/('i-'+'b'*32)
        if phase in ('prepared','staged'):
            source.write_text('interrupted conversion')
            if phase=='staged': indexed(store,obj,obj)
        else:
            obj.write_text('interrupted conversion');indexed(store,source,obj)
        if phase in ('counted','target-staged'):
            pathlib.Path(str(obj)+'.count').write_bytes((1).to_bytes(8,'little'))
        if phase=='target-staged': indexed(store,store/'new',obj)
        (store/'conversion').write_bytes(str(source).encode()+b'\0'+str(obj).encode()+b'\0')
        survivor=store/('i-'+'a'*32);survivor.write_text('live')
        indexed(store,root/'survivor',survivor)
        pathlib.Path(str(survivor)+'.count').write_bytes((1).to_bytes(8,'little'))
        args=[str(out/'link-deny'),str(out/'linux-layer-v3-linktest'),'--android-filter',str(out/'hardlinks-test'),str(root),'--recover']
        result=subprocess.run(args,capture_output=True,text=True,timeout=15)
        assert result.returncode==0,(phase,result.stderr)
        assert source.exists(),phase
        assert not (store/'new').exists() and not (store/'conversion').exists()
        assert len(list(store.glob('p-*')))==(1 if phase in ('prepared','staged') else 2),phase
        result=subprocess.run(args[:3]+['/bin/cat',str(source)],capture_output=True,text=True,timeout=15)
        assert result.returncode==0 and result.stdout=='interrupted conversion',(phase,result.stderr)
print('PASS five exchange checkpoints preserve source data and retire unpublished indices')
