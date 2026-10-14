#!/usr/bin/env python3
"""Unix transport backs actual glibc DNS; no external network/server needed."""
import pathlib,socket,subprocess,tempfile,threading,struct,time
root=pathlib.Path(__file__).resolve().parents[1]
for mode in ('udp','tcp','timeout'):
    with tempfile.TemporaryDirectory(prefix='v3-i386-resolver-') as tmp:
        path=tmp+'/dns';conf=pathlib.Path(tmp)/'resolv.conf'
        conf.write_text('nameserver 127.10.42.99\noptions timeout:1 attempts:1\n')
        queries=[];errors=[];stop=threading.Event()
        udp=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);udp.bind(path+'.udp');udp.listen(8);udp.settimeout(.1)
        tcp=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);tcp.bind(path+'.tcp');tcp.listen(8);tcp.settimeout(.1)
        def reply(q,truncated=False):
            return q[:2]+(b'\x83\x80' if truncated else b'\x81\x80')+q[4:6]+(b'\0\0' if truncated else b'\0\1')+b'\0\0\0\0'+q[12:]+(b'' if truncated else b'\xc0\x0c\0\1\0\1\0\0\0\x3c\0\4\xc0\0\2\1')
        def exact(c,n):
            b=b''
            while len(b)<n:
                part=c.recv(n-len(b))
                if not part:raise EOFError()
                b+=part
            return b
        def service(listener,stream):
            while not stop.is_set():
                try:c,_=listener.accept()
                except socket.timeout:continue
                except OSError:return
                with c:
                    c.settimeout(2)
                    try:
                        while not stop.is_set():
                            q=exact(c,struct.unpack('!H',exact(c,2))[0]) if stream else c.recv(65535)
                            if not q:break
                            queries.append('tcp' if stream else 'udp')
                            if mode=='timeout':continue
                            out=reply(q,mode=='tcp' and not stream)
                            c.sendall(struct.pack('!H',len(out))+out if stream else out)
                    except (socket.timeout,EOFError,ConnectionResetError):pass
                    except Exception as e:errors.append(e)
        workers=[threading.Thread(target=service,args=(udp,False)),threading.Thread(target=service,args=(tcp,True))]
        for w in workers:w.start()
        started=time.monotonic()
        try:
            result=subprocess.run([str(root/'build/linux-layer-v3-i386'),'--android-filter',
                '--map',str(conf),'/etc/resolv.conf','--dns-forwarder','127.10.42.99:53','--dns-socket',path,
                str(root/'build/i386-p1/resolver'),'timeout' if mode=='timeout' else 'answer'],capture_output=True,timeout=8)
            (root/f'build/i386-p1/resolver-{mode}.log').write_bytes(result.stderr)
            assert result.returncode==0,(mode,result.returncode,result.stderr)
            assert queries and not errors,(queries,errors)
            if mode=='tcp':assert 'tcp' in queries,queries
            if mode=='timeout':assert .5<time.monotonic()-started<7
            print('PASS i386 getaddrinfo '+mode)
        finally:
            stop.set();udp.close();tcp.close()
            for w in workers:w.join(3)
