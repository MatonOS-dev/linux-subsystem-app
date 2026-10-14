#!/usr/bin/env python3
"""Real resolver calls through Unix transport under the copied app filter."""
import os, pathlib, socket, struct, subprocess, tempfile, threading, sys
root=pathlib.Path(__file__).resolve().parents[1]
subprocess.run(['cc','-no-pie','-O2','-Wall','-Wextra','-Werror','-pthread',str(root/'tests/dns.c'),'-lresolv','-o',str(root/'build/dns-test')],check=True)
def answer(data):
    return data[:2]+b'\x81\x80'+data[4:6]+b'\x00\x01\x00\x00\x00\x00'+data[12:]+b'\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04\xc0\x00\x02\x01'
def exact(client,n):
    data=b''
    while len(data)<n:
        chunk=client.recv(n-len(data))
        if not chunk:raise EOFError()
        data+=chunk
    return data
def client_worker(client,stream):
    with client:
        try:
            _,uid,_=struct.unpack('3i',client.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))
            assert uid==os.getuid()
            while True:
                data=exact(client,struct.unpack('!H',exact(client,2))[0]) if stream else client.recv(65535)
                if not data:return
                # Request name 'truncate.test' exercises automatic TCP fallback.
                if b'timeout' in data:continue
                reply=answer(data)
                if b'truncate' in data and not stream:reply=data[:2]+b'\x83\x80'+data[4:6]+b'\0'*6+data[12:]
                client.sendall(struct.pack('!H',len(reply))+reply) if stream else client.send(reply)
        except (OSError,EOFError):pass
def serve(server,stream):
    while True:
        try:
            client,_=server.accept();threading.Thread(target=client_worker,args=(client,stream),daemon=True).start()
        except OSError:return
with tempfile.TemporaryDirectory(prefix='v3-dns-',dir='/tmp') as tmp:
    path=str(pathlib.Path(tmp)/'dns');servers=[]
    for suffix,kind in [('.udp',socket.SOCK_SEQPACKET),('.tcp',socket.SOCK_STREAM)]:
        server=socket.socket(socket.AF_UNIX,kind)
        try: server.bind(path+suffix)
        except PermissionError:
            if '--skip-unavailable' not in sys.argv:raise
            print('SKIP Unix DNS integration: host sandbox forbids AF_UNIX socket bind')
            sys.exit(0)
        server.listen(32);servers.append(server)
        threading.Thread(target=serve,args=(server,suffix=='.tcp'),daemon=True).start()
    base=[str(root/'build/linux-layer-v3'),'--android-filter','--dns-forwarder','127.10.42.99:53','--dns-socket',path]
    try:
        for mode in ['glibc','tcp','fallback','musl-sendto','peer','sendmsg','sendmmsg','recvmsg','poll','pre-poll','epoll','alias','threads','multiple','timeout']:
            result=subprocess.run(base+[str(root/'build/dns-test'),mode],capture_output=True,text=True,timeout=20)
            (root/f'build/dns-{mode}.log').write_text(result.stderr)
            assert result.returncode==0,(mode,result.stdout,result.stderr)
            print('PASS Unix DNS',mode)
        for endpoint in ['1.2.3.4:53','127.10.42.99:5353','127.10.42.999:53','127.10.42.99:65536','127.10.42.99:']:
            assert subprocess.run([str(root/'build/linux-layer-v3'),'--dns-forwarder',endpoint,'/bin/true'],stderr=subprocess.DEVNULL).returncode==2
        print('PASS invalid endpoint rejection')
    finally:
        for server in servers:server.close()
