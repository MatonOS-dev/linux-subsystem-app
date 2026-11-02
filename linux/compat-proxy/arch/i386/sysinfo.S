.section .proxy_gateway,"ax",@progbits
.global proxy_i386_sysinfo_entry
.hidden proxy_i386_sysinfo_entry
.type proxy_i386_sysinfo_entry,@function
proxy_i386_sysinfo_entry:
    /* Preserve the vDSO contract: syscall arguments and every register other
     * than EAX survive, and the caller's flags are restored before RET. */
    pushfl
    pushal
    mov 28(%esp),%eax
    mov %cs,%cx
    cmp $0x23,%cx                 /* non-flat LDT code keeps the trap route */
    jne .Ltrap
    cmp $221,%eax                 /* fcntl64: only non-locking commands */
    je .Lfcntl
    cmp $453,%eax
    jae .Ltrap
    call .Lmap_pc
.Lmap_pc:
    pop %edx
    bt %eax,.Ldirect_map-.Lmap_pc(%edx)
    jc .Ldirect
    jmp .Ltrap
.Lfcntl:
    mov 16(%esp),%edx             /* EBX: command */
    cmp $0,%edx                   /* F_DUPFD */
    je .Ldirect
    cmp $1,%edx                   /* F_GETFD */
    je .Ldirect
    cmp $2,%edx                   /* F_SETFD */
    je .Ldirect
    cmp $3,%edx                   /* F_GETFL */
    je .Ldirect
    cmp $4,%edx                   /* F_SETFL */
    je .Ldirect
    cmp $1030,%edx                /* F_DUPFD_CLOEXEC */
    je .Ldirect
    jmp .Ltrap
.Ldirect:
    /* Build the canonical six-word argument array for the shared dispatcher. */
    sub $48,%esp
    mov 64(%esp),%eax
    mov %eax,0(%esp)              /* EBX */
    movl $0,4(%esp)
    mov 72(%esp),%eax
    mov %eax,8(%esp)              /* ECX */
    movl $0,12(%esp)
    mov 68(%esp),%eax
    mov %eax,16(%esp)             /* EDX */
    movl $0,20(%esp)
    mov 52(%esp),%eax
    mov %eax,24(%esp)             /* ESI */
    movl $0,28(%esp)
    mov 48(%esp),%eax
    mov %eax,32(%esp)             /* EDI */
    movl $0,36(%esp)
    mov 56(%esp),%eax
    mov %eax,40(%esp)             /* EBP */
    movl $0,44(%esp)
    mov %esp,%edi                 /* keep the saved-register/argument base */
    mov 76(%edi),%eax
    and $-16,%esp
    push %edi
    push %eax
    call proxy_i386_sysinfo_dispatch
    mov %eax,76(%edi)             /* popal returns the syscall result in EAX */
    lea 48(%edi),%esp
    popal
    popfl
    ret
.Ltrap:
    popal
    popfl
    /* PIC tail-call into the separately mapped payload trampoline. The
     * resident DSO's own int80 would be exempt in a dynamic bionic peer. */
    push %eax
    call .Lpayload_pc
.Lpayload_pc:
    pop %eax
    lea proxy_i386_payload_sysinfo-.Lpayload_pc(%eax),%eax
    mov (%eax),%eax
    xchg %eax,(%esp)
    ret
/* Bitmap of numbers taking the direct dispatcher (the x86_64 rewrite set):
 * 6 close, 9 link, 10 unlink, 15 chmod, 33 access, 38 rename, 39 mkdir, 40 rmdir, 83 symlink, 85 readlink, 140 _llseek, 183 getcwd, 195 stat64, 196 lstat64, 197 fstat64, 198 lchown32, 212 chown32, 220 getdents64, 295 openat, 296 mkdirat, 297 mknodat, 298 fchownat, 300 fstatat64, 301 unlinkat, 302 renameat, 303 linkat, 304 symlinkat, 305 readlinkat, 306 fchmodat, 307 faccessat, 320 utimensat, 353 renameat2, 383 statx, 439 faccessat2, 452 fchmodat2. */
.Ldirect_map:
    .byte 0x40,0x86,0x00,0x00,0xc2,0x01,0x00,0x00,0x00,0x00,0x28,0x00,0x00,0x00,0x00,0x00
    .byte 0x00,0x10,0x00,0x00,0x00,0x00,0x80,0x00,0x78,0x00,0x10,0x10,0x00,0x00,0x00,0x00
    .byte 0x00,0x00,0x00,0x00,0x80,0xf7,0x0f,0x00,0x01,0x00,0x00,0x00,0x02,0x00,0x00,0x80
    .byte 0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x00,0x10
.size proxy_i386_sysinfo_entry,.-proxy_i386_sysinfo_entry
.hidden proxy_i386_payload_sysinfo
.section .note.GNU-stack,"",@progbits
