#!/usr/bin/env python3
"""Check APK entries/extraction using real NDK ELF executables, not stub bytes.
Android-linked loader images are extracted/inspected here; host execution and
trusted-sibling behavior are covered by the separate Linux loader fixtures.
"""
import pathlib,zipfile,subprocess,tempfile,os,stat,shutil
root=pathlib.Path(__file__).resolve().parents[1]
names=('libmatonos-v3-peer-x86_64.so','libmatonos-v3-loader.so','libmatonos-v3-loader-i386.so','libmatonos-v3-loader-static.so','libmatonos-v3-peer-i386.so')
with tempfile.TemporaryDirectory(prefix='packaging-',dir=root/'build') as tmp:
    directory=pathlib.Path(tmp);apk=directory/'fixture.apk';out=directory/'extracted'
    with zipfile.ZipFile(apk,'w') as z:
        for name in names:z.write(root/'build/android-test'/name,'lib/x86_64/'+name,compress_type=zipfile.ZIP_STORED)
        # The production host APK also carries the Flatpak planner helper;
        # this fixture only exercises extraction, so a loader ELF is sufficient.
        z.write(root/'build/android-test'/names[0],'lib/x86_64/libmatonos-flatpak-plan.so',compress_type=zipfile.ZIP_STORED)
    subprocess.run(['bash',str(root/'extract-loaders.sh'),str(apk),str(out)],check=True)
    for name,elf_class in zip(names,(2,2,1,2,1)):
        p=out/name;assert p.read_bytes()==(root/'build/android-test'/name).read_bytes()
        assert stat.S_IMODE(p.stat().st_mode)==0o755
        assert p.read_bytes()[:5]==b'\x7fELF'+bytes([elf_class])
    print('PASS NDK loaders extracted from APK entries as trusted 0755 files')
    broken=directory/'missing.apk'
    with zipfile.ZipFile(broken,'w') as z:z.write(root/'build/android-test'/names[0],'lib/x86_64/'+names[0])
    result=subprocess.run(['bash',str(root/'extract-loaders.sh'),str(broken),str(directory/'missing')],capture_output=True)
    assert result.returncode!=0
    print('PASS extraction rejects a missing i386 peer')
# Trusted sibling rejection uses real loaders with a deliberately untrusted peer.
with tempfile.TemporaryDirectory(prefix='peer-reject-',dir=root/'build') as tmp:
    d=pathlib.Path(tmp);os.chmod(d,0o755)
    for name in ('linux-layer-v3','linux-layer-v3-i386'):shutil.copy2(root/'build'/name,d/name)
    args=[str(d/'linux-layer-v3'),'--android-filter',str(root/'build/cross32'),str(root/'build/cross64'),str(root/'build/cross32')]
    peer=d/'linux-layer-v3-i386'
    for case in ('writable','symlink','wrong-class','directory'):
        if peer.exists() or peer.is_symlink():peer.unlink()
        shutil.copy2(root/'build/linux-layer-v3-i386',peer)
        if case=='writable':os.chmod(peer,0o777)
        elif case=='symlink':peer.unlink();peer.symlink_to(root/'build/linux-layer-v3-i386')
        elif case=='wrong-class':shutil.copy2(root/'build/linux-layer-v3',peer)
        else:os.chmod(d,0o777)
        result=subprocess.run(args,capture_output=True,timeout=5)
        assert result.returncode==1,(case,result.returncode,result.stderr)
        os.chmod(d,0o755)
    print('PASS trusted sibling rejects writable files/directories, symlinks and wrong ELF class')
