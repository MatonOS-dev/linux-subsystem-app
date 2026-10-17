"""First-conversion publication checkpoints, constructed by the store owner.
The killed-operation replay test separately exercises a real SIGKILL intent.
"""
import pathlib, subprocess, tempfile
base=pathlib.Path(__file__).resolve().parents[1]

def indexed(store, path, obj):
    path.touch();path.chmod(0o1600);st=path.stat()
    (store/f'p-{st.st_dev:016x}-{st.st_ino:016x}').write_bytes(obj.name.encode()+b'\0')

for phase in ('prepared','moved','unindexed','indexed','published','counted'):
    with tempfile.TemporaryDirectory(prefix='conversion-',dir=base/'build') as tmp:
        root=pathlib.Path(tmp).resolve();(root/'.v3-store-root').touch()
        store=root/'.v3-inodes';store.mkdir(mode=0o700)
        source=root/'restored';obj=store/('i-'+'b'*32)
        (source if phase=='prepared' else obj).write_text('interrupted conversion')
        if phase=='unindexed':
            (store/'new').touch();(store/'new').chmod(0o1600)
        if phase=='indexed': indexed(store,store/'new',obj)
        if phase in ('published','counted'): indexed(store,source,obj)
        if phase=='counted': pathlib.Path(str(obj)+'.count').write_bytes((1).to_bytes(8,'little'))
        (store/'conversion').write_bytes(str(source).encode()+b'\0'+str(obj).encode()+b'\0')
        survivor=store/('i-'+'a'*32);survivor.write_text('live')
        indexed(store,root/'survivor',survivor)
        pathlib.Path(str(survivor)+'.count').write_bytes((1).to_bytes(8,'little'))
        args=[str(base/'build/link-deny'),str(base/'build/linux-layer-v3'),'--android-filter',str(base/'build/hardlinks-test'),str(root),'--recover']
        result=subprocess.run(args,capture_output=True,text=True,timeout=15)
        assert result.returncode==0,(phase,result.stderr)
        assert not (store/'new').exists() and not (store/'conversion').exists()
        assert len(list(store.glob('p-*')))==(2 if phase in ('published','counted') else 1),phase
        # Verify recovered contents through an independently started proxy.
        result=subprocess.run(args[:3]+['/bin/cat',str(source)],capture_output=True,text=True,timeout=15)
        assert result.returncode==0 and result.stdout=='interrupted conversion',(phase,result.stderr)
print('PASS all six conversion checkpoints preserve content and discard unpublished placeholder indices')
