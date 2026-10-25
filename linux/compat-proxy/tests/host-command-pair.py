#!/usr/bin/env python3
"""Same resident worker over inherited socketpair (runner disallows connect)."""
import array, os, pathlib, socket, struct, subprocess, tempfile, time, pty, select
ROOT=pathlib.Path(__file__).resolve().parents[1]
subprocess.run(['cc','-DHOST_COMMAND_TEST','-nostdlib','-static','-no-pie','-mcmodel=large','-Wl,-Ttext-segment=0x600000000000','-Wl,--section-start=.proxy_gateway=0x600000300000','-O2','-fno-stack-protector','-fno-builtin','-Wall','-Wextra','-Werror',*[str(ROOT/s) for s in ['loader.c','proxy.c','vfs.c','links.c','net.c','sysvipc.c', 'state.c','raw.S','entry.S']],'-o',str(ROOT/'build/host-command-loader')],check=True)
def read_int(s):
    b=b''
    while len(b)<4:
        n=s.recv(4-len(b))
        if not n:raise RuntimeError('EOF')
        b+=n
    return struct.unpack('<i',b)[0]
def parent(pid):
    for line in pathlib.Path(f'/proc/{pid}/status').read_text().splitlines():
        if line.startswith('PPid:'):return int(line.split()[1])
def stop_tree(pid):
    children=pathlib.Path(f'/proc/{pid}/task/{pid}/children')
    if children.exists():
        for child in children.read_text().split():stop_tree(int(child))
    try:os.kill(pid,15)
    except ProcessLookupError:pass
with tempfile.TemporaryDirectory(prefix='hc-pair-',dir='/mnt/data/aosp/out') as tmp:
    d=pathlib.Path(tmp);(d/'app').mkdir();(d/'app'/'marker').write_text('sandbox-marker\n');(d/'info').write_text('[Application]\nname=test\n')
    reserved=os.open("/dev/null",os.O_RDONLY);assert reserved==3
    active=[]
    def spawn(argv,cwd='/app',env=None,fds=None,flags=0):
        env=env or {};fds=fds or {};a,b=socket.socketpair();os.dup2(b.fileno(),3);b.close();a.settimeout(5)
        original=os.environ.copy();original['HC_INHERIT']='inherited'
        log=open(d/'log','ab')
        p=subprocess.Popen([str(ROOT/'build/host-command-loader'),'--android-filter','--trace','--host-command-fd','3','/usr/bin/bwrap','--sync-fd','3','--tmpfs','/','--ro-bind','/usr','/usr','--symlink','usr/bin','/bin','--symlink','usr/lib','/lib','--symlink','usr/lib64','/lib64','--proc','/proc','--dev','/dev','--bind',str(d/'app'),'/app','--ro-bind',str(d/'info'),'/.flatpak-info','--chdir','/app','/bin/sleep','60'],pass_fds=[3],env=original,stdout=log,stderr=log);os.close(3);assert os.open("/dev/null",os.O_RDONLY)==3;active.append((p,log))
        body=b''.join(struct.pack('<I',n) for n in fds)+cwd.encode()+b'\0'+b''.join(x.encode()+b'\0' for x in argv)+b''.join((k+'='+v).encode()+b'\0' for k,v in env.items())
        packet=struct.pack('<6I',0x314d4348,len(body),len(argv),len(env),len(fds),flags)+body
        a.sendmsg([packet],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',fds.values()))] if fds else [])
        return a,read_int(a),p
    try:
        with open(d/'out','w+b') as out:
            s,pid,p=spawn(['/bin/sh','-c','printf "%s:%s:" "$HC_INHERIT" "$HC_OVERRIDE"; cat marker; pwd; exit 7'],env={'HC_OVERRIDE':'merged'},fds={1:out.fileno(),2:out.fileno()})
            assert pid>0,(pid,(d/'log').read_text());assert read_int(s)==7<<8;s.close();out.seek(0);text=out.read();assert text==b'inherited:merged:sandbox-marker\n/app\n',text
        with open(d/'out','w+b') as out:
            s,pid,p=spawn(['/bin/sh','-c','printf "%s:%s" "${HC_INHERIT-unset}" "$ONLY"'],env={'ONLY':'clean'},fds={1:out.fileno()},flags=1)
            assert pid>0 and read_int(s)==0;s.close();out.seek(0);assert out.read()==b'unset:clean'
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tests/host-command-child.c'),'-o',str(d/'app/hc-child')],check=True)
        master,slave=pty.openpty()
        try:
            s,pid,p=spawn(['/app/hc-child'],env={'EXPECTED_UID':str(os.getuid())},fds={0:slave,1:slave,2:slave,42:slave})
            assert pid>0,pid
            assert select.select([master],[],[],5)[0], 'pty child did not start'
            result=os.read(master,4096);assert b'PTY_READY' in result,result
            os.write(master,b'hello\n');assert read_int(s)==0;s.close()
            result=os.read(master,4096);assert b'PTY_INPUT:hello' in result,result
        finally:os.close(master);os.close(slave)
        for argv,cwd in [(['/absent'],'/app'),(['/bin/true'],'/absent')]:
            s,pid,p=spawn(argv,cwd=cwd);assert pid==-2,pid;s.close()
        s,pid,p=spawn(['/bin/sleep','60']);assert pid>0;os.write(s.fileno(),struct.pack('<II',15,1));assert read_int(s)==15;s.close()
        s,pid,p=spawn(['/bin/sleep','60'],flags=2);assert pid>0;s.close()
        for _ in range(50):
            if not pathlib.Path(f'/proc/{pid}').exists():break
            time.sleep(.05)
        assert not pathlib.Path(f'/proc/{pid}').exists(),'WATCH_BUS survived'
        s,pid,p=spawn(['/bin/sleep','60']);assert pid>0;s.close();time.sleep(.2);assert pathlib.Path(f'/proc/{pid}').exists()
        os.kill(parent(parent(pid)),15);p.wait(5)
        for _ in range(50):
            if not pathlib.Path(f'/proc/{pid}').exists():break
            time.sleep(.05)
        assert not pathlib.Path(f'/proc/{pid}').exists(),'runtime child survived'
        s,pid,p=spawn(['/bin/sh','-c','/usr/bin/setsid /bin/sleep 60 & echo $! > /app/detached-pid']);assert pid>0;assert read_int(s)==0;s.close()
        detached=int((d/'app/detached-pid').read_text());time.sleep(.2);assert pathlib.Path(f'/proc/{detached}').exists(),'detached child ended before runtime'
        os.kill(parent(parent(detached)),15);p.wait(5)
        for _ in range(50):
            if not pathlib.Path(f'/proc/{detached}').exists():break
            time.sleep(.05)
        assert not pathlib.Path(f'/proc/{detached}').exists(),'detached session survived runtime'
        print('PASS pty stdio/control tty, repeated descriptor at fd42, actual UID, detached-session cleanup')
        print('PASS resident worker under app filter: cwd /app, SCM_RIGHTS, env merge/clear, spawn failures, raw wait status, signals, WATCH_BUS, runtime death')
    finally:
        for p,log in active:
            if p.poll() is None:stop_tree(p.pid);p.wait(5)
            log.close()
        os.close(3)
        if __import__('sys').exc_info()[0] and (d/'log').exists():print((d/'log').read_text())
