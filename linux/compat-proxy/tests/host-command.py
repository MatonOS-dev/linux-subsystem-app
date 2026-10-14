#!/usr/bin/env python3
"""Resident sandbox spawn, real SCM_RIGHTS, copied Android filter; no adb."""
import array, os, pathlib, signal, socket, struct, subprocess, tempfile, time
ROOT=pathlib.Path(__file__).resolve().parents[1]

def read_int(s):
    b=b''
    while len(b)<4:
        chunk=s.recv(4-len(b))
        if not chunk: raise RuntimeError('supervisor EOF')
        b+=chunk
    return struct.unpack('<i',b)[0]

def spawn(path,argv,cwd='/app',env=None,fds=None,flags=0):
    env=env or {}; fds=fds or {}
    body=b''.join(struct.pack('<I',n) for n in fds)+cwd.encode()+b'\0'
    body+=b''.join(a.encode()+b'\0' for a in argv)
    body+=b''.join((k+'='+v).encode()+b'\0' for k,v in env.items())
    frame=struct.pack('<6I',0x314d4348,len(body),len(argv),len(env),len(fds),flags)+body
    s=socket.socket(socket.AF_UNIX);s.settimeout(8);s.connect(str(path))
    anc=[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',fds.values()))] if fds else []
    s.sendmsg([frame],anc);pid=read_int(s)
    return s,pid

def parent(pid):
    for line in pathlib.Path(f'/proc/{pid}/status').read_text().splitlines():
        if line.startswith('PPid:'):return int(line.split()[1])
def stop_tree(pid):
    children=pathlib.Path(f'/proc/{pid}/task/{pid}/children')
    if children.exists():
        for child in children.read_text().split():stop_tree(int(child))
    try:os.kill(pid,15)
    except ProcessLookupError:pass

with tempfile.TemporaryDirectory(prefix='host-command-',dir='/mnt/data/aosp/out') as tmp:
    d=pathlib.Path(tmp);(d/'app').mkdir();(d/'app'/'marker').write_text('sandbox-marker\n');(d/'info').write_text('[Application]\nname=test\n')
    sock=d/'spawn';log=open(d/'log','w')
    env=os.environ.copy();env['HC_INHERIT']='inherited'
    p=subprocess.Popen([str(ROOT/'build/linux-layer-v3'),'--android-filter','--host-command',str(sock),'/usr/bin/bwrap','--tmpfs','/','--ro-bind','/usr','/usr','--symlink','usr/bin','/bin','--symlink','usr/lib','/lib','--symlink','usr/lib64','/lib64','--proc','/proc','--dev','/dev','--bind',str(d/'app'),'/app','--ro-bind',str(d/'info'),'/.flatpak-info','--chdir','/app','/bin/sleep','60'],env=env,stdout=log,stderr=log)
    try:
        for _ in range(100):
            if sock.exists():break
            if p.poll() is not None:raise RuntimeError((d/'log').read_text())
            time.sleep(.05)
        with open(d/'out','w+b') as out:
            s,pid=spawn(sock,['/bin/sh','-c','printf "%s:%s:" "$HC_INHERIT" "$HC_OVERRIDE"; cat marker; pwd; exit 7'],env={'HC_OVERRIDE':'merged'},fds={1:out.fileno(),2:out.fileno()})
            assert pid>0,pid
            assert read_int(s)==7<<8
            s.close();out.seek(0);text=out.read();assert text==b'inherited:merged:sandbox-marker\n/app\n',text
        with open(d/'out','w+b') as out:
            s,pid=spawn(sock,['/bin/sh','-c','printf "%s:%s" "${HC_INHERIT-unset}" "$ONLY"'],env={'ONLY':'clean'},fds={1:out.fileno()},flags=1)
            assert pid>0 and read_int(s)==0;s.close();out.seek(0);assert out.read()==b'unset:clean'
        s,pid=spawn(sock,['/does-not-exist']);assert pid==-2,pid;s.close()
        s,pid=spawn(sock,['/bin/true'],cwd='/absent');assert pid==-2,pid;s.close()
        s,pid=spawn(sock,['/bin/sleep','60']);assert pid>0,pid;s.sendall(struct.pack('<II',15,1));assert read_int(s)==15;s.close()
        s,pid=spawn(sock,['/bin/sleep','60'],flags=2);assert pid>0;s.close()
        for _ in range(80):
            if not pathlib.Path(f'/proc/{pid}').exists():break
            time.sleep(.05)
        assert not pathlib.Path(f'/proc/{pid}').exists(),'WATCH_BUS child survived'
        s,pid=spawn(sock,['/bin/sleep','60'],flags=0);assert pid>0;s.close();time.sleep(.2);assert pathlib.Path(f'/proc/{pid}').exists(),'non-WATCH child killed'
        os.kill(parent(parent(parent(pid))),15);p.wait(timeout=5)
        for _ in range(80):
            if not pathlib.Path(f'/proc/{pid}').exists():break
            time.sleep(.05)
        assert not pathlib.Path(f'/proc/{pid}').exists(),'runtime child survived'
        print('PASS sandbox /app/cwd, env merge/clear, SCM_RIGHTS, raw wait status, spawn errors, group signal, WATCH_BUS, runtime death')
    finally:
        if p.poll() is None:stop_tree(p.pid);p.wait(timeout=5)
        log.close()
