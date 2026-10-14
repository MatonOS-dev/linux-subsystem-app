#!/usr/bin/env python3
"""Flatpak fallback decision, /etc alias, bwrap binds, DNS and HostCommand."""
import array, os, pathlib, socket, struct, subprocess, tempfile, threading, time, sys
ROOT = pathlib.Path(__file__).resolve().parents[1]
loader = str(ROOT/'build/linux-layer-v3')
# Offline checks require no permitted host sockets.
if '--offline' in sys.argv:
    subprocess.run(['cc','-no-pie','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tests/resolv.c'),'-lresolv','-o',str(ROOT/'build/resolv-test')],check=True)
    with tempfile.TemporaryDirectory(prefix='v3-resolv-',dir='/mnt/data/aosp/out') as tmp:
        d=pathlib.Path(tmp); root=d/'root';root.mkdir();(d/'system/etc').mkdir(parents=True)
        for name in ('usr','bin','lib','lib64','sbin','proc','dev'):(root/name).symlink_to('/'+name)
        (root/'system').symlink_to(d/'system');(root/'etc').symlink_to('/system/etc')
        resolv=d/'resolv.conf';resolv.write_text('nameserver 127.10.42.99\n')
        info=d/'info';info.write_text('[Application]\nname=test\n')
        base=[loader,'--android-filter','--root',str(root),'--map','/mnt','/mnt','--map',str(resolv),'/etc/resolv.conf']
        assert subprocess.run(base+['/bin/sh','-c','test -e /etc/resolv.conf'],stderr=subprocess.DEVNULL).returncode==1
        base+=['--map',str(resolv),'/system/etc/resolv.conf']
        assert subprocess.run(base+['/bin/sh','-c','test -e /etc/resolv.conf'],stderr=subprocess.DEVNULL).returncode==0
        print('PASS /etc symlink reproduces missing map; canonical map fixes fallback decision')
        for isolated, monitor in ((False,False),(True,False),(False,True),(True,True)):
            args=['/usr/bin/bwrap','--tmpfs','/','--ro-bind','/usr','/usr','--symlink','usr/bin','/bin','--symlink','usr/lib','/lib','--symlink','usr/lib64','/lib64','--dir','/etc','--ro-bind','/etc/resolv.conf','/etc/resolv.conf','--ro-bind',str(info),'/.flatpak-info','--ro-bind',str(ROOT/'build/resolv-test'),'/test','--proc','/proc','--dev','/dev']
            if monitor:
                index=args.index('--ro-bind', args.index('--dir')+2)
                args[index:index+3]=['--ro-bind',str(d),'/run/host/monitor','--symlink','/run/host/monitor/resolv.conf','/etc/resolv.conf']
            if isolated:args+=['--unshare-net']
            result=subprocess.run(base+args+['/test','isolated' if isolated else 'network'],capture_output=True,text=True,timeout=20)
            assert result.returncode==0,result.stderr
            print(result.stdout.strip())
    sys.exit(0)
dns_tmp = tempfile.TemporaryDirectory(prefix='v3-resolv-dns-',dir='/tmp')
dns_path = str(pathlib.Path(dns_tmp.name)/'forwarder')
server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
server.bind(dns_path + '.udp');server.listen(32)
queries = []
def dns_client(client):
    with client:
        try:
            while True:
                data = client.recv(65535)
                if not data:return
                queries.append(data)
                reply = data[:2]+b'\x81\x80'+data[4:6]+b'\x00\x01\x00\x00\x00\x00'+data[12:]+b'\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04\xc0\x00\x02\x01'
                client.send(reply)
        except OSError:return
def dns():
    while True:
        try:
            client,_ = server.accept()
            threading.Thread(target=dns_client,args=(client,),daemon=True).start()
        except OSError:return
threading.Thread(target=dns, daemon=True).start()
def integer(s):
    data=b''
    while len(data)<4:
        part=s.recv(4-len(data))
        assert part, 'HostCommand EOF'
        data+=part
    return struct.unpack('<i',data)[0]
def host_command(path, command, out):
    argv=['/bin/sh','-c',command]
    body=struct.pack('<II',1,2)+b'/\0'+b''.join(a.encode()+b'\0' for a in argv)
    frame=struct.pack('<6I',0x314d4348,len(body),len(argv),0,2,0)+body
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(10);s.connect(str(path))
        s.sendmsg([frame],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',[out.fileno(),out.fileno()]))])
        assert integer(s)>0
        assert integer(s)==0
with tempfile.TemporaryDirectory(prefix='v3-resolv-',dir='/mnt/data/aosp/out') as tmp:
    d=pathlib.Path(tmp); scaffold=d/'root';scaffold.mkdir()
    (d/'system/etc').mkdir(parents=True)
    for name in ('usr','bin','lib','lib64','sbin','proc','dev'):
        (scaffold/name).symlink_to('/'+name)
    (scaffold/'system').symlink_to(d/'system')
    (scaffold/'etc').symlink_to('/system/etc')
    resolv=d/'resolv.conf';resolv.write_text('nameserver 127.10.42.99\noptions timeout:1 attempts:1\n')
    info=d/'info';info.write_text('[Application]\nname=test\n')
    base=[loader,'--android-filter','--root',str(scaffold),'--map','/mnt','/mnt','--dns-forwarder','127.10.42.99:53','--dns-socket',dns_path,'--map',str(resolv),'/etc/resolv.conf']
    # Reproduce the host existence test used by add_monitor_path_args().
    assert subprocess.run(base+['/bin/sh','-c','test -e /etc/resolv.conf'],stderr=subprocess.DEVNULL).returncode==1
    base+=['--map',str(resolv),'/system/etc/resolv.conf']
    assert subprocess.run(base+['/bin/sh','-c','test -e /etc/resolv.conf'],stderr=subprocess.DEVNULL).returncode==0
    print('PASS missing resolver reproduced and canonical host map fixes Flatpak fallback decision')
    for isolated in (False,True):
        path=d/('spawn-isolated' if isolated else 'spawn-network')
        bwrap=['/usr/bin/bwrap','--tmpfs','/','--ro-bind','/usr','/usr','--symlink','usr/bin','/bin','--symlink','usr/lib','/lib','--symlink','usr/lib64','/lib64','--dir','/etc','--ro-bind','/etc/resolv.conf','/etc/resolv.conf','--ro-bind',str(info),'/.flatpak-info','--proc','/proc','--dev','/dev']
        if isolated:bwrap+=['--unshare-net']
        check=('set -e; test ! -e /etc/resolv.conf; ! getent ahostsv4 example.test' if isolated else 'set -e; cat /etc/resolv.conf; getent ahostsv4 example.test')
        with open(d/'log','w') as log:
            p=subprocess.Popen(base+['--host-command',str(path)]+bwrap+['/bin/sh','-c',check+'; exec sleep 30'],stdout=log,stderr=log)
            try:
                for _ in range(100):
                    if path.exists():break
                    assert p.poll() is None,(d/'log').read_text()
                    time.sleep(.05)
                assert path.exists(),(d/'log').read_text()
                before=len(queries)
                with open(d/'child','w+b') as out:
                    host_command(path,check,out);out.seek(0);result=out.read()
                if isolated:
                    assert len(queries)==before
                    assert b'192.0.2.1' not in result
                else:
                    assert b'nameserver 127.10.42.99' in result and b'192.0.2.1' in result,result
                    assert len(queries)>before
                p.terminate();p.wait(timeout=5)
                text=(d/'log').read_text()
                if not isolated:assert '192.0.2.1' in text,text
                print('PASS', 'no-network resolver hidden and DNS denied' if isolated else 'app and HostCommand getaddrinfo use mapped resolver and Unix DNS transport')
            finally:
                if p.poll() is None:p.kill();p.wait()
server.close()
dns_tmp.cleanup()
