#!/usr/bin/env python3
"""Replay the entire logged app bundle in order, replacing unavailable VM inputs.
No deployment is changed. Sources/fds are host fixtures; destinations/options
and payload are the logged ones. Extra lifecycle fds exercise monitor behavior.
"""
import hashlib, importlib.util, json, os, pathlib, re, subprocess
base = pathlib.Path(__file__).resolve().parents[1]
build = base/'build'
fixture = build/'logged-bwrap'
fixture.mkdir(exist_ok=True)
log = pathlib.Path(os.environ['COMPAT_STAGE2_LOG']).read_text()
bundles = re.findall(r'v3 bwrap bundle begin\n(.*?)v3 bwrap bundle end', log, re.S)
args = bundles[-1].splitlines()
assert '--info-fd' in args and '/etc/ld.so.conf' in args
original = args[:]
outer = log.split('v3 bwrap bundle end')[-1]
logged_command = re.findall(r'^v3 bwrap argv (.*)$', outer, re.M)[1:]
assert logged_command == ['--','bash','-c','ls /app /usr; head -2 /etc/os-release']
runtime = pathlib.Path(os.environ['COMPAT_HOST_RUNTIME'])
spec = importlib.util.spec_from_file_location('flatpak_fixture', base/'tests/flatpak-bwrap-host.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
opened=[]
def keep(fd):
    opened.append(fd)
    return str(fd)
def data(content):
    fd=os.memfd_create('logged-flatpak',0)
    os.write(fd,content);os.lseek(fd,0,0)
    return keep(fd)
def directory(path):
    path.mkdir(parents=True,exist_ok=True)
    return path

def source(target):
    if target=='/usr':return runtime
    if target.startswith('/etc/'):
        candidate=runtime/target.lstrip('/')
        if candidate.exists():return candidate
    path=fixture/'sources'/target.lstrip('/')
    # Missing files from runtime 25.08 get deterministic inert host substitutes.
    if target.startswith('/etc/') and target not in ['/etc/security','/etc/profile.d','/etc/rc_keymaps','/etc/fonts','/etc/alsa','/etc/gtk-3.0','/etc/xdg','/etc/pulse','/etc/ssl','/etc/debuginfod','/etc/pki']:
        path.parent.mkdir(parents=True,exist_ok=True);path.write_text('fixture\n');return path
    directory(path)
    if target=='/app':(path/'.ref').touch()
    return path
# Protect actual host sources; hash all runtime etc files before/after.
def digest_etc():
    return {str(p.relative_to(runtime)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (runtime/'etc').rglob('*') if p.is_file()}
before=digest_etc()
fdmap={};info=None
arity0={'--unshare-user','--disable-userns','--unshare-pid','--unshare-net','--unshare-ipc','--die-with-parent','--new-session','--as-pid-1','--clearenv'}
arity1={'--lock-file','--proc','--dir','--perms','--tmpfs','--remount-ro','--info-fd','--seccomp','--cap-drop','--cap-add','--chdir','--hostname','--uid','--gid'}
arity2={'--ro-bind-fd','--bind-fd','--ro-bind-data','--file','--ro-bind','--bind','--dev-bind','--ro-bind-try','--symlink','--setenv'}
i=0
while i<len(args):
    flag=args[i]
    if flag in arity0:n=0
    elif flag in arity1:n=1
    elif flag in arity2:n=2
    else:raise RuntimeError('unknown logged option '+flag)
    if flag in ['--ro-bind-fd','--bind-fd']:
        old=args[i+1];path=source(args[i+2])
        if old not in fdmap:fdmap[old]=keep(os.open(path,os.O_RDONLY))
        args[i+1]=fdmap[old]
    elif flag in ['--ro-bind-data','--file','--seccomp']:
        old=args[i+1];target=args[i+2] if n==2 else ''
        if flag=='--seccomp':content=module.flatpak_filter_bytes()
        elif target=='/etc/ld.so.conf':content=b'/lib\n/lib/x86_64-linux-gnu\ninclude /run/flatpak/ld.so.conf.d/*.conf\n'
        elif target=='/etc/ld.so.cache':content=b''
        elif target=='/etc/passwd':content=b'user:x:1000:1000:User:/home/user:/bin/bash\n'
        elif target=='/etc/group':content=b'user:x:1000:\n'
        else:content=b'fixture\n'
        if old not in fdmap:fdmap[old]=data(content)
        args[i+1]=fdmap[old]
    elif flag=='--info-fd':
        info=os.open(fixture/'info.json',os.O_CREAT|os.O_TRUNC|os.O_RDWR,0o600)
        args[i+1]=keep(info)
    elif flag in ['--ro-bind','--bind','--dev-bind','--ro-bind-try']:
        src=args[i+1];target=args[i+2]
        if not src.startswith(('/proc/','/sys/')) and src!='/dev':
            path=fixture/'paths'/src.lstrip('/')
            if target.endswith(('.ref','per-app-dirs-ref')) or src=='/etc/hosts':
                path.parent.mkdir(parents=True,exist_ok=True);path.write_text('127.0.0.1 localhost\n' if src=='/etc/hosts' else '')
            else:directory(path)
            if '/.flatpak/' in src and src.rsplit('/',1)[-1].isdigit():(path/'.ref').touch()
            args[i+1]=str(path)
    i+=n+1
status=os.open(fixture/'status.json',os.O_CREAT|os.O_TRUNC|os.O_RDWR,0o600)
sync_read,sync_write=os.pipe();block_read,block_write=os.pipe()
args+=['--json-status-fd',keep(status),'--sync-fd',keep(sync_write),'--block-fd',keep(block_read),'--cap-drop','ALL']
command=logged_command
android_data=directory(fixture/'android-data'/'matonos'/'linux')
(android_data/'machine-id').write_text('0123456789abcdef0123456789abcdef\n')
directory(android_data/'udev')
argv=module.proxy+['--map',str(fixture/'android-data'),'/data']+[module.bwrap,'--args',data(b'\0'.join(x.encode() for x in args)+b'\0')]+command
(fixture/'argv.json').write_text(json.dumps({'logged':original,'host':args,'command':command},indent=2)+'\n')
try:
    with open(fixture/'out','wb') as out,open(fixture/'log','wb') as err:
        proc=subprocess.Popen(argv,pass_fds=opened,stdout=out,stderr=err,env={'PATH':'/usr/bin:/bin','TMPDIR':str(fixture)})
        import time
        deadline=time.monotonic()+20
        while proc.poll() is None and time.monotonic()<deadline:
            os.lseek(info,0,0)
            try:
                if 'child-pid' in json.loads(os.read(info,65536)):break
            except json.JSONDecodeError:pass
            time.sleep(.01)
        else:
            proc.kill();proc.wait();raise RuntimeError('no info-fd setup; see '+str(fixture/'log'))
        assert out.tell()==0,'payload ran before block-fd release'
        os.write(block_write,b'1');os.close(block_write);block_write=-1
        try:code=proc.wait(timeout=40)
        except subprocess.TimeoutExpired:proc.kill();proc.wait();raise
    assert code==0,f'exit {code}: see {fixture}/log'
    os.lseek(status,0,0);events=[json.loads(s) for s in os.read(status,65536).splitlines()]
    assert any(e.get('exit-code')==0 for e in events),events
    os.close(sync_write);opened.remove(sync_write)
    assert os.read(sync_read,1)==b''
    output=(fixture/'out').read_text();assert 'NAME=' in output and '/usr:' in output,output
    assert digest_etc()==before,'runtime source modified'
    print('PASS full logged app bundle, payload, filter, info/status/block/sync fds, cap drop; runtime etc unchanged')
finally:
    if block_write>=0:os.close(block_write)
    os.close(sync_read)
    for fd in opened:os.close(fd)
