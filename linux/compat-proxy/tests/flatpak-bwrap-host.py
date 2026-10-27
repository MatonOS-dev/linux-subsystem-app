#!/usr/bin/env python3
"""Offline Flatpak 1.19 bwrap phases; no installed deployment is changed.
The fd bundle and base args follow common/flatpak-{run,bwrap}.c. The ldconfig
phase uses the host runtime; the payload phase adds lifecycle/status/filter fds.
"""
import ctypes, errno, fcntl, json, os, pathlib, re, shlex, subprocess, time
base = pathlib.Path(__file__).resolve().parents[1]
build = base / 'build'
root = build / 'musl-host-root'
fixture = build / 'flatpak-bwrap'
fixture.mkdir(exist_ok=True)
proxy = [str(build/'linux-layer-v3'), '--android-filter', '--map', str(root/'system_ext'), '/system_ext']
runtime = pathlib.Path(os.environ.get('COMPAT_HOST_RUNTIME', '/usr'))
bwrap = '/system_ext/priv-app/MatonLinuxSubsystem/lib/x86_64/bwrap'
# The VM log exposes the outer command, but not the contents of --args.
# Replay the actual ldconfig payload when supplied, retaining source-derived
# host fd bundles: VM backing files and memfds cannot be read offline.
stage2_log = os.environ.get('COMPAT_STAGE2_LOG')
ldconfig_command = ['ldconfig','-X','-C','/run/ld-so-cache-dir/cache']
if stage2_log:
    log = pathlib.Path(stage2_log).read_text()
    matches = re.findall(r"F: Running: '([^\n]*matonos-app-bwrap --args [^\n]*)'", log)
    commands = [shlex.split(line) for line in matches]
    commands = [args[3:] for args in commands if args[3:4] == ['ldconfig']]
    if len(commands) != 1: raise RuntimeError('expected one logged ldconfig invocation')
    ldconfig_command = commands[0]
    if ldconfig_command[:3] != ['ldconfig','-X','-C'] or not ldconfig_command[3].startswith('/run/ld-so-cache-dir/'):
        raise RuntimeError('unexpected Stage 2 ldconfig command')

def flatpak_filter_bytes(diagnostic=False):
    # setup_seccomp() in Flatpak 1.19.2, x86_64, nondevel, nonmultiarch,
    # no CAN/Bluetooth. Export without installing it in the Python runner.
    lib=ctypes.CDLL('libseccomp.so.2')
    class Compare(ctypes.Structure):
        _fields_=[('arg',ctypes.c_uint),('op',ctypes.c_int),
                  ('a',ctypes.c_uint64),('b',ctypes.c_uint64)]
    lib.seccomp_init.argtypes=[ctypes.c_uint32];lib.seccomp_init.restype=ctypes.c_void_p
    lib.seccomp_release.argtypes=[ctypes.c_void_p]
    lib.seccomp_syscall_resolve_name.argtypes=[ctypes.c_char_p]
    lib.seccomp_export_bpf.argtypes=[ctypes.c_void_p,ctypes.c_int]
    for name in ['seccomp_rule_add_array','seccomp_rule_add_exact_array']:
        getattr(lib,name).argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_int,
                                   ctypes.c_uint,ctypes.POINTER(Compare)]
    ctx=lib.seccomp_init(0x7fff0000)
    if not ctx:raise RuntimeError('seccomp_init failed')
    fd=os.memfd_create('flatpak-filter',0)
    def rule(name,error,comparison=None,exact=False):
        nr=lib.seccomp_syscall_resolve_name(name.encode())
        cmp=Compare(*comparison) if comparison else None
        fn=lib.seccomp_rule_add_exact_array if exact else lib.seccomp_rule_add_array
        result=fn(ctx,0x50000|error,nr,int(cmp is not None),ctypes.byref(cmp) if cmp else None)
        if result and result!=-errno.EFAULT:raise RuntimeError(f'seccomp rule {name}: {result}')
    try:
        for name in ['syslog','uselib','acct','quotactl','add_key','keyctl','request_key',
                     'move_pages','mbind','get_mempolicy','set_mempolicy','migrate_pages',
                     'unshare','setns','mount','umount','umount2','pivot_root','chroot',
                     'perf_event_open','ptrace','modify_ldt']:
            rule(name,errno.EPERM)
        rule('clone',errno.EPERM,(0,7,0x10000000,0x10000000))
        for request in [0x5412,0x541c]:rule('ioctl',errno.EPERM,(1,7,0xffffffff,request))
        for name in ['clone3','open_tree','move_mount','fsopen','fsconfig','fsmount','fspick','mount_setattr']:
            rule(name,errno.ENOSYS)
        rule('personality',errno.EPERM,(0,1,0,0))
        for family in range(17):
            if family not in [0,1,2,10,16]:rule('socket',errno.EAFNOSUPPORT,(0,4,family,0),True)
        rule('socket',errno.EAFNOSUPPORT,(0,5,17,0),True)
        if diagnostic:rule('getppid',errno.EACCES)
        result=lib.seccomp_export_bpf(ctx,fd)
        if result:raise RuntimeError(f'seccomp_export_bpf: {result}')
        os.lseek(fd,0,0);return os.read(fd,65536)
    finally:os.close(fd);lib.seccomp_release(ctx)

def phase(name, payload=False, compat=False, backing=True):
    launch_proxy=list(proxy)
    opened=[]
    def data(content):
        fd=os.memfd_create('flatpak-'+name, os.MFD_ALLOW_SEALING); opened.append(fd)
        os.write(fd, content); os.lseek(fd,0,0)
        fcntl.fcntl(fd,fcntl.F_ADD_SEALS,fcntl.F_SEAL_SEAL|fcntl.F_SEAL_SHRINK|fcntl.F_SEAL_GROW|fcntl.F_SEAL_WRITE)
        return str(fd)
    def directory(path):
        pathlib.Path(path).mkdir(exist_ok=True)
        fd=os.open(path,os.O_RDONLY|os.O_DIRECTORY);opened.append(fd);return str(fd)
    runtime_fd=directory(runtime)
    app_fd=directory(fixture/'app')
    (fixture/'app/.ref').touch()
    if payload:
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',
                        str(base/'tests/proc-dirfd.c'),'-o',str(fixture/'app/proc-dirfd')],check=True)
    cache_fd=directory(fixture/'cache')
    args=['--ro-bind-fd',runtime_fd,'/usr','--ro-bind-fd',app_fd,'/app',
          '--unshare-user','--disable-userns','--proc','/proc','--unshare-pid',
          '--dir','/tmp','--dir','/var/tmp','--dir','/run/host',
          '--perms','0700','--dir',f'/run/user/{os.getuid()}',
          '--setenv','XDG_RUNTIME_DIR',f'/run/user/{os.getuid()}',
          '--symlink','../run','/var/run',
          '--ro-bind-try','/proc/self/ns/user','/run/.userns',
          '--symlink','/etc/timezone','/var/db/zoneinfo',
          '--ro-bind-data',data(b'user:x:1000:1000:User:/home/user:/bin/bash\n'),'/etc/passwd',
          '--ro-bind-data',data(b'user:x:1000:\n'),'/etc/group',
          '--ro-bind-data',data(b'user-config: none\n'),'/etc/pkcs11/pkcs11.conf']
    if compat:
        # Optional extensions have a disposable files/ path separate from the
        # immutable image. bwrap must reach the image through that path before
        # creating tmpfs and GL32 destinations below its app-side bind alias.
        (fixture/'app/lib/i386-linux-gnu').mkdir(parents=True,exist_ok=True)
        deployment=fixture/'compat-deployment/files'
        image=fixture/'compat-image/26.08'
        deployment.mkdir(parents=True,exist_ok=True)
        image.mkdir(parents=True,exist_ok=True)
        launch_proxy+=['--map-ro',str(deployment),str(deployment),
                       '--erofs-mount','/usr/lib/i386-linux-gnu',str(image)]
        if backing:
            launch_proxy+=['--erofs-backing','/usr/lib/i386-linux-gnu',str(deployment)]
        args+=['--ro-bind',str(deployment),'/app/lib/i386-linux-gnu',
               '--tmpfs','/app/lib/i386-linux-gnu/GL',
               '--dir','/app/lib/i386-linux-gnu/GL/default']
    for path in ['/sys/block','/sys/bus','/sys/class','/sys/dev','/sys/devices']:
        args+=['--ro-bind',path,path]
    for path in ['bin','lib','lib64','sbin']:
        if (runtime/path).exists(): args+=['--symlink','usr/'+path,'/'+path]
    # Host /usr lacks a runtime etc directory. Supply only the loader config;
    # no host ld.so.cache is bound, so ldconfig must generate the fixture cache.
    args+=['--ro-bind-data',data(b'/lib\n/lib/x86_64-linux-gnu\n'),'/etc/ld.so.conf',
           '--unshare-ipc','--unshare-net','--proc','/proc','--dev','/dev',
           '--bind-fd',cache_fd,'/run/ld-so-cache-dir','--setenv','PATH','/usr/bin:/usr/sbin',
           '--setenv','HOME','/home/user']
    if (runtime/'.ref').exists():args+=['--lock-file','/usr/.ref']
    args+=['--lock-file','/app/.ref']
    checks={}
    if payload:
        # Instance metadata and locks follow flatpak_run_add_app_info_args.
        instance=fixture/'instance';instance.mkdir(exist_ok=True)
        (instance/'.ref').touch()
        extension_conf='runtime-001-org.freedesktop.Platform.GL.maton.conf'
        (instance/'ld.so.conf.d').mkdir(exist_ok=True)
        (instance/'ld.so.conf.d'/extension_conf).touch()
        (instance/'info').write_text('[Application]\nname=org.matonos.CompatTest\n')
        (instance/'run-environ').write_bytes(b'PATH=/usr/bin:/bin\0container=flatpak\0')
        args+=['--ro-bind',str(instance),'/run/flatpak',
               '--lock-file','/run/flatpak/.ref']
        # Flatpak binds generated extension loader config files below this
        # long destination. bwrap resolves the parent through its tracked fd
        # and then mounts the --ro-bind-data tmpfile at the reconstructed
        # path; keep this exact shape covered against fd-path truncation.
        args+=['--ro-bind-data',data(b'/usr/lib\n'),
               '/run/flatpak/ld.so.conf.d/'+extension_conf]
        for flag in ['--info-fd','--json-status-fd']:
            fd=os.open(fixture/(name+flag+'.json'),os.O_RDWR|os.O_CREAT|os.O_TRUNC,0o600)
            opened.append(fd);checks[flag]=fd;args += [flag,str(fd)]
        block_read,block_write=os.pipe();opened += [block_read,block_write]
        # Parent retains the writer; the child blocks until info-fd is complete.
        opened.remove(block_write)
        args+=['--block-fd',str(block_read)]
        sync_read,sync_write=os.pipe();opened += [sync_read,sync_write]
        args+=['--sync-fd',str(sync_write)]
        info=b'[Application]\nname=org.matonos.CompatTest\n'
        args+=['--perms','0600','--file',data(info),'/.flatpak-info',
               '--ro-bind-data',data(info),'/.flatpak-info',
               '--setenv','container','flatpak']
        command=['/bin/bash','-c','ls /app /usr; head -2 /etc/passwd; cat /run/flatpak/info; test "$(cat /run/flatpak/ld.so.conf.d/runtime-001-org.freedesktop.Platform.GL.maton.conf)" = /usr/lib; cmp /.flatpak-info "/proc/$$/root/.flatpak-info"; echo proc-root-ok; /app/proc-dirfd || exit $?; /app/filter-check']
    else:
        command=ldconfig_command
    args+=['--seccomp',data(flatpak_filter_bytes(payload))]
    bundle=data(b'\0'.join(x.encode() for x in args)+b'\0')
    argv=launch_proxy+[bwrap,'--args',bundle]+command
    (fixture/(name+'.argv.json')).write_text(json.dumps({'expanded_bwrap': [bwrap]+args+command,'exec':argv},indent=2)+'\n')
    try:
        with open(build/('musl-flatpak-'+name+'.out'),'wb') as out,open(build/('musl-flatpak-'+name+'.log'),'wb') as log:
            proc=subprocess.Popen(argv, pass_fds=opened, stdout=out, stderr=log,env={'PATH':'/usr/bin:/bin','TMPDIR':str(fixture)})
            try:
                if payload:
                    deadline=time.monotonic()+15
                    while True:
                        try:
                            os.lseek(checks['--info-fd'],0,0)
                            info=json.loads(os.read(checks['--info-fd'],65536))
                            if 'child-pid' in info:break
                        except json.JSONDecodeError: pass
                        if proc.poll() is not None or time.monotonic()>=deadline:
                            raise RuntimeError('bwrap did not report setup through info-fd')
                        time.sleep(.01)
                    assert proc.poll() is None and out.tell()==0, 'payload ran before block-fd release'
                    os.write(block_write,b'1');os.close(block_write);block_write=-1
                result=proc.wait(timeout=40)
            finally:
                if proc.poll() is None:proc.kill();proc.wait()
        bundle_log=(build/('musl-flatpak-'+name+'.log')).read_text()
        assert 'v3 bwrap bundle begin\n' in bundle_log and 'v3 bwrap bundle end\n' in bundle_log
        if compat and not backing:
            diagnostic=(build/('musl-flatpak-'+name+'.log')).read_text()
            assert result==1 and "Can't create file /app/lib/i386-linux-gnu/GL" in diagnostic,diagnostic
            print('PASS missing optional extension backing reproduces bwrap failure before ldconfig')
            return
        if result: raise RuntimeError(f'{name}: exit {result}; see musl-flatpak-{name}.log')
        for flag,fd in checks.items():
            os.lseek(fd,0,0);content=os.read(fd,65536).decode()
            if flag=='--info-fd': assert 'child-pid' in json.loads(content),content
            else:
                events=[json.loads(line) for line in content.splitlines()]
                assert any('child-pid' in e for e in events),events
                assert any(e.get('exit-code')==0 for e in events),events
        if payload:
            os.close(sync_write);opened.remove(sync_write)
            assert os.read(sync_read,1)==b'', 'bwrap monitor retained sync fd'
            payload_out=(build/('musl-flatpak-'+name+'.out')).read_bytes()
            assert b'filter-ok' in payload_out
            assert b'proc-root-ok' in payload_out
            assert b'PASS proc dirfd' in payload_out
        else: assert (fixture/'cache'/pathlib.Path(command[3]).name).stat().st_size>0
        print('PASS static musl Flatpak-shaped '+name)
    finally:
        if payload and block_write>=0:os.close(block_write)
        for fd in opened:os.close(fd)
if __name__ == '__main__':
    phase('compat-missing-backing',compat=True,backing=False)
    phase('compat-ldconfig',compat=True)
    phase('ldconfig')
    phase('payload',True)
