#!/usr/bin/env python3
"""Reject forged/replayed/unsealed/truncated private exec-state streams."""
import os,fcntl,struct,pathlib
root=pathlib.Path(__file__).resolve().parents[1]
for loader in ('linux-layer-v3','linux-layer-v3-i386'):
    for case in ('unsealed','version','pid','nonce','truncated','oversize'):
        pid=os.fork()
        if pid==0:
            fd=os.memfd_create('invalid-exec-state',os.MFD_ALLOW_SEALING)
            os.set_inheritable(fd,True)
            data=struct.pack('<4Q',0x335854454e4f544d,2 if case=='version' else 3,
                             os.getpid()+1 if case=='pid' else os.getpid(),19)
            os.write(fd,data[:20] if case=='truncated' else data)
            if case=='oversize':os.ftruncate(fd,(64<<20)+1)
            if case!='unsealed':fcntl.fcntl(fd,fcntl.F_ADD_SEALS,fcntl.F_SEAL_SEAL|fcntl.F_SEAL_SHRINK|fcntl.F_SEAL_GROW|fcntl.F_SEAL_WRITE)
            os.execv(str(root/'build'/loader),[loader,'--resident-state',str(fd),'20' if case=='nonce' else '19','/bin/true','/bin/true'])
        _,status=os.waitpid(pid,0)
        assert os.waitstatus_to_exitcode(status)==2,(loader,case,status)
    print('PASS '+loader+' rejects invalid private exec state')
