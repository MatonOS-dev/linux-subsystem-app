#!/usr/bin/env python3
"""Exercise the production Java tar/index/layer writer with pinned mkfs/fsck."""
import os, pathlib, subprocess, sys, tempfile

ROOT=pathlib.Path(__file__).resolve().parents[1]
STUB=ROOT.parent/'stubgen'
MKFS=os.environ.get('MKFS_EROFS',str(ROOT/'prebuilt/x86_64/matonos-mkfs-erofs'))
FSCK=os.environ.get('FSCK_EROFS','fsck.erofs')
sys.path.insert(0,str(ROOT));from erofs_layers import read_index

sources=[STUB/'src/org/matonos/linuxhost/stubgen/ObjectPacks.java',
         STUB/'src/org/matonos/linuxhost/stubgen/OstreePacks.java',
         STUB/'src/org/matonos/linuxhost/stubgen/PlaceholderFiles.java',
         STUB/'src/org/matonos/linuxhost/stubgen/PackLaunch.java',
         STUB/'src/org/matonos/linuxhost/stubgen/ErofsLayers.java',
         ROOT/'tests/ErofsLayersHarness.java',ROOT/'tests/OstreeTimeHarness.java']
with tempfile.TemporaryDirectory(prefix='matonos-erofs-java-') as td:
    out=pathlib.Path(td);classes=out/'classes';classes.mkdir()
    subprocess.run(['javac','-d',str(classes),*map(str,sources)],check=True)
    repo=out/'repo';tree=out/'tree';tree.mkdir();(tree/'sample').write_text('commit metadata')
    subprocess.run(['ostree','init','--repo='+str(repo),'--mode=bare-user'],check=True,stdout=subprocess.DEVNULL)
    subprocess.run(['ostree','commit','--repo='+str(repo),'-b','time-test','--tree=dir='+str(tree),'-m','time'],check=True,stdout=subprocess.DEVNULL)
    commit=subprocess.check_output(['ostree','rev-parse','--repo='+str(repo),'time-test'],text=True).strip()
    subprocess.run(['java','-cp',str(classes),'OstreeTimeHarness',str(repo),commit],check=True)
    subprocess.run(['java','-cp',str(classes),'ErofsLayersHarness',str(out),str(pathlib.Path(MKFS).resolve())],check=True)
    bundle=out/'B';layers,rows=read_index(bundle/'layers.index');dest=out/'extract';dest.mkdir();images=[]
    for i,layer in enumerate(layers):
        image=bundle/('pack-'+layer['sha256']+'.erofs');root=out/f'layer-{i}';root.mkdir()
        subprocess.run([FSCK,'--extract='+str(root),'--preserve','--xattrs',str(image)],check=True,stdout=subprocess.DEVNULL)
        images.append(root)
    paths={r['path']:r for r in rows}
    for row in rows:
        if row['kind']=='dir':continue
        target=dest/row['path'].lstrip('/');target.parent.mkdir(parents=True,exist_ok=True)
        if row['kind']=='symlink':target.symlink_to(row['target'])
        else:
            source=images[row['layer']]/row['path'].lstrip('/');target.write_bytes(source.read_bytes())
            assert target.read_bytes()==source.read_bytes()
    assert paths['/usr/share/doc/file-link']['kind']=='symlink'
    assert '/usr/share/doc/gone' not in paths and '/usr/share/doc/deleted-dir/child' not in paths
    print('PASS Java layers.index extraction: '+str(sum(x['image_size'] for x in layers))+' image bytes')
