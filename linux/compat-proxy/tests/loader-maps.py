#!/usr/bin/env python3
"""Inspect real kernel VMAs, sharing and permission-denied copy fallbacks."""
import pathlib,subprocess,select,os
base=pathlib.Path(__file__).resolve().parents[1]
root=base/'build/loader-maps';root.mkdir(exist_ok=True)
def run(*a):subprocess.run(list(map(str,a)),check=True)
run('cc','-O2','-Wall','-Wextra','-Werror',base/'tests/mmap-deny.c','-o',root/'deny')
run('cc','-O2','-Wall','-Wextra','-Werror','-DREFUSAL=EPERM',base/'tests/mmap-deny.c','-o',root/'deny-perm')
for kind,flags in [('dynamic',[]),('static',['-static'])]:
 run('cc',*flags,base/'tests/loader-maps.c','-o',root/kind)
ndk=pathlib.Path(os.environ.get('MATON_ANDROID_NDK',os.environ.get('MATON_NDK',str(pathlib.Path.home()/'Documents/android-ndk-r30'))))
compiler=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android36-clang'
run(compiler,'-O2','-fno-stack-protector','-c',base/'tests/loader-maps.c','-o',root/'maps32.o')
run('cc','-m32','-c',base/'tests/i386-entry.S','-o',root/'entry32.o')
run('ld','-m','elf_i386','-o',root/'dynamic32','--dynamic-linker',os.environ.get('MATON_V3_I386_INTERPRETER','/lib/ld-linux.so.2'),root/'entry32.o',root/'maps32.o',os.environ.get('MATON_V3_I386_LIBC','/usr/lib32/libc.so.6'))
proxy=base/'build/linux-layer-v3'
def inspect(args,needle,refused=False):
 processes=[]
 try:
  for _ in range(2):
   log=(root/('log-'+str(len(processes)))).open('w+')
   p=subprocess.Popen(list(map(str,([root/('deny-perm' if refused=='EPERM' else 'deny')] if refused else [])+[proxy,'--android-filter']+args)),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log)
   processes.append((p,log))
   assert select.select([p.stdout],[],[],15)[0], 'payload startup timeout'
   assert p.stdout.readline()==b'ready\n', 'payload failed startup'
  for p,log in processes:
   maps=pathlib.Path(f'/proc/{p.pid}/maps').read_text()
   (root/f'maps-{p.pid}').write_text(maps)
   text=[line for line in maps.splitlines() if 'r-xp' in line and needle in line]
   if not refused:
    assert text,(needle,maps)
    smaps=pathlib.Path(f'/proc/{p.pid}/smaps').read_text()
    (root/f'smaps-{p.pid}').write_text(smaps)
    for line in text:
     block=smaps.split(line+'\n',1)[1].split('VmFlags:',1)[0]
     assert 'Private_Dirty:         0 kB' in block,block
   else:
    assert not text,(needle,text)
    log.flush();log.seek(0);assert 'exec-mmap-copy' in log.read()
  for p,_ in processes:
   p.stdin.write(b'q');p.stdin.flush();assert p.wait(timeout=10)==0
 finally:
  for p,log in processes:
   if p.poll() is None:p.kill();p.wait()
   log.close()
for kind in ('static','dynamic'):
 for refused in (False,'EACCES','EPERM'):
  inspect([root/kind],str(root/kind),refused)
  inspect(['/bin/sh','-c','exec '+str(root/kind)],str(root/kind),refused)
  print(kind,'resident load/exec', str(refused)+' copy fallback' if refused else 'file-backed clean text','PASS',flush=True)
bundle=base/'build/pack-host/bundle'
for refused in (False,'EACCES','EPERM'):
 inspect(['--pack-mount','/packed',bundle/'objects.index',bundle,'/packed/maps'],'.zip',refused)
 print('pack main + dynamic linker', str(refused)+' copy fallback' if refused else 'file-backed clean text','PASS',flush=True)

for refused in (False,'EACCES','EPERM'):
 for loader in ('linux-layer-v3','linux-layer-v3-i386'):
  proxy=base/'build'/loader
  inspect([root/'dynamic32'],str(root/'dynamic32'),refused)
  print(loader,'i386 dynamic', str(refused)+' copy fallback' if refused else 'file-backed clean text','PASS',flush=True)
