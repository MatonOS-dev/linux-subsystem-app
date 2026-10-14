#!/usr/bin/env python3
import pathlib,socket,subprocess,tempfile,threading
root=pathlib.Path(__file__).resolve().parents[1]
for arch,peer in ((64,32),(32,64)):
    with tempfile.TemporaryDirectory(prefix='v3-cross-dns-') as tmp:
        path=tmp+'/dns';queries=[]
        with socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET) as server:
            server.bind(path+'.udp');server.listen(1);server.settimeout(5)
            def answer():
                with server.accept()[0] as client:
                    client.settimeout(5)
                    for _ in range(2):
                        q=client.recv(4096);queries.append(q);client.send(q[:2]+b'\x81\x80'+q[4:])
            worker=threading.Thread(target=answer,daemon=True);worker.start()
            loader='linux-layer-v3' if arch==64 else 'linux-layer-v3-i386'
            result=subprocess.run([str(root/'build'/loader),'--android-filter','--trace',
                '--dns-forwarder','127.10.42.99:53','--dns-socket',path,
                str(root/f'build/cross-dns{arch}'),str(root/f'build/cross-dns{peer}'),str(root/f'build/cross-dns{arch}')],capture_output=True,timeout=8)
            (root/f'build/cross-dns{arch}.log').write_bytes(result.stderr)
            assert result.returncode==0,(arch,result.returncode,result.stderr)
            worker.join(2);assert len(queries)==2
            print(result.stdout.decode(),end='')
