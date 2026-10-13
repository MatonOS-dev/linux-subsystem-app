#!/usr/bin/env python3
"""Real glibc res_nquery plus musl-style unconnected sendto and message APIs."""
import pathlib, socket, struct, subprocess, threading
root=pathlib.Path(__file__).resolve().parents[1]
subprocess.run(['cc','-no-pie','-O2','-Wall','-Wextra','-Werror',str(root/'tests/dns.c'),'-lresolv','-o',str(root/'build/dns-test')],check=True)
sockets=[]
def answer(data):
    return data[:2]+b'\x81\x80'+data[4:6]+b'\x00\x01\x00\x00\x00\x00'+data[12:]+b'\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04\xc0\x00\x02\x01'
def udp(server):
    while True:
        try:
            data,peer=server.recvfrom(65535);server.sendto(answer(data),peer)
        except OSError:return
def exact(client,n):
    data=b''
    while len(data)<n:
        chunk=client.recv(n-len(data))
        if not chunk:raise EOFError()
        data+=chunk
    return data
def tcp(server):
    while True:
        try:
            client,_=server.accept()
            with client:
                size=struct.unpack('!H',exact(client,2))[0];data=answer(exact(client,size));client.sendall(struct.pack('!H',len(data))+data)
        except OSError:return
for address in ['127.10.42.99','127.10.42.100']:
    for kind,worker in [(socket.SOCK_DGRAM,udp),(socket.SOCK_STREAM,tcp)]:
        server=socket.socket(socket.AF_INET,kind);server.bind((address,5353))
        if kind==socket.SOCK_STREAM:server.listen()
        sockets.append(server);threading.Thread(target=worker,args=(server,),daemon=True).start()
try:
    for mode in ['glibc','tcp','musl-sendto','peer','sendmsg','sendmmsg','native']:
        result=subprocess.run([str(root/'build/linux-layer-v3'),'--android-filter','--dns-forwarder','127.10.42.99:5353',str(root/'build/dns-test'),mode],capture_output=True,text=True,timeout=10)
        (root/f'build/dns-{mode}.log').write_text(result.stderr)
        assert result.returncode==0,(mode,result.stdout,result.stderr)
        print('PASS',mode)
    for endpoint in ['1.2.3.4:5353','127.10.42.99:53','127.10.42.999:5353','127.10.42.99:65536','127.10.42.99:']:
        assert subprocess.run([str(root/'build/linux-layer-v3'),'--dns-forwarder',endpoint,'/bin/true']).returncode==2
    print('PASS invalid endpoint rejection')
finally:
    for server in sockets:server.close()
