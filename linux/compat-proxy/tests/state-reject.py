#!/usr/bin/env python3
"""Reject forged/replayed/unsealed/truncated private exec-state streams."""
import os,fcntl,struct,pathlib
root=pathlib.Path(__file__).resolve().parents[1]
# A rejection before the authenticated header used to exit 2 without a trace:
# the fresh resident has no trace flag and no imported sink. Each stage must
# reach the inherited fd 2, and resident entry itself must be visible.
stages={'unsealed':b'resident-import-seals','version':b'resident-import-version',
        'pid':b'resident-import-pid','nonce':b'resident-import-nonce',
        'truncated':b'resident-import-size'}
for loader in ('linux-layer-v3','linux-layer-v3-i386'):
    for case in ('unsealed','version','pid','nonce','truncated','oversize'):
        readfd,writefd=os.pipe()
        pid=os.fork()
        if pid==0:
            os.close(readfd)
            os.dup2(writefd,2)
            os.close(writefd)
            fd=os.memfd_create('invalid-exec-state',os.MFD_ALLOW_SEALING)
            os.set_inheritable(fd,True)
            data=struct.pack('<4Q',0x335854454e4f544d,2 if case=='version' else 11,
                             os.getpid()+1 if case=='pid' else os.getpid(),19)
            os.write(fd,data[:20] if case=='truncated' else data)
            if case=='oversize':os.ftruncate(fd,(64<<20)+1)
            if case!='unsealed':fcntl.fcntl(fd,fcntl.F_ADD_SEALS,fcntl.F_SEAL_SEAL|fcntl.F_SEAL_SHRINK|fcntl.F_SEAL_GROW|fcntl.F_SEAL_WRITE)
            os.execv(str(root/'build'/loader),[loader,'--resident-state',str(fd),'20' if case=='nonce' else '19','/bin/true','/bin/true'])
        os.close(writefd)
        output=b''
        while True:
            block=os.read(readfd,4096)
            if not block:break
            output+=block
        os.close(readfd)
        _,status=os.waitpid(pid,0)
        assert os.waitstatus_to_exitcode(status)==2,(loader,case,status)
        assert b'resident-enter' in output,(loader,case,output)
        if case in stages:assert stages[case] in output,(loader,case,output)
    print('PASS '+loader+' rejects invalid private exec state')

# A valid handoff header followed by a truncated VFS body must report failure
# to the preserved sink even when the exec caller redirected fd 2.
for loader in ('linux-layer-v3','linux-layer-v3-i386'):
    readfd,writefd=os.pipe()
    pid=os.fork()
    if pid==0:
        os.close(readfd)
        os.set_inheritable(writefd,True)
        fd=os.memfd_create('truncated-vfs-state',os.MFD_ALLOW_SEALING)
        os.set_inheritable(fd,True)
        os.write(fd,struct.pack('<7Q',0x335854454e4f544d,11,os.getpid(),19,1,writefd,0))
        fcntl.fcntl(fd,fcntl.F_ADD_SEALS,fcntl.F_SEAL_SEAL|fcntl.F_SEAL_SHRINK|fcntl.F_SEAL_GROW|fcntl.F_SEAL_WRITE)
        null=os.open('/dev/null',os.O_WRONLY)
        os.dup2(null,2)
        os.close(null)
        os.execv(str(root/'build'/loader),[loader,'--resident-state',str(fd),'19','/bin/true','/bin/true'])
    os.close(writefd)
    output=b''
    while True:
        block=os.read(readfd,4096)
        if not block:break
        output+=block
    os.close(readfd)
    _,status=os.waitpid(pid,0)
    assert os.waitstatus_to_exitcode(status)==2,(loader,status)
    assert b'exec-state-enter' in output and b'exec-state-vfs-error' in output,(loader,output)
    print('PASS '+loader+' reports early import failure through redirected stderr')
