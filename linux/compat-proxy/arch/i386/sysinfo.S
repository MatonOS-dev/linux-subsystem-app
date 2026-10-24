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
    cmp $6,%eax                   /* close */
    je .Ldirect
    cmp $85,%eax                  /* readlink */
    je .Ldirect
    cmp $140,%eax                 /* _llseek */
    je .Ldirect
    cmp $195,%eax                 /* stat64 */
    je .Ldirect
    cmp $197,%eax                 /* fstat64 */
    je .Ldirect
    cmp $220,%eax                 /* getdents64 */
    je .Ldirect
    cmp $221,%eax                 /* fcntl64: only non-locking commands */
    je .Lfcntl
    cmp $295,%eax                 /* openat */
    je .Ldirect
    cmp $300,%eax                 /* fstatat64 */
    je .Ldirect
    cmp $305,%eax                 /* readlinkat */
    je .Ldirect
    cmp $383,%eax                 /* statx */
    je .Ldirect
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
    int $0x80
    ret
.size proxy_i386_sysinfo_entry,.-proxy_i386_sysinfo_entry
.section .note.GNU-stack,"",@progbits
