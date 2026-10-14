#!/usr/bin/env python3
import pathlib,socket,subprocess,tempfile,threading
root=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='v3-i386-dns-') as tmp:
    path=tmp+'/dns'
    with socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET) as server:
        server.bind(path+'.udp');server.listen(1)
        def answer():
            with server.accept()[0] as client:
                q=client.recv(4096);client.send(q[:2]+b'\x81\x80'+q[4:])
        worker=threading.Thread(target=answer,daemon=True);worker.start()
        result=subprocess.run([str(root/'build/linux-layer-v3-i386'),'--android-filter',
            '--dns-forwarder','127.10.42.99:53','--dns-socket',path,
            str(root/'build/i386-p1/dns')],capture_output=True,timeout=10)
        (root/'build/i386-p1/dns.log').write_bytes(result.stderr)
        assert result.returncode==0,(result.returncode,result.stderr)
        print(result.stdout.decode(),end='')
