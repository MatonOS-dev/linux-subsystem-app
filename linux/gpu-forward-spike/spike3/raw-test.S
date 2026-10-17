.text
.global _start
_start:
    mov %rsp,%rdi
    and $-16,%rsp
    call raw_main
    ud2
.global raw_restore
raw_restore:
    mov $15,%eax
    syscall
    ud2
.global raw_clone
raw_clone:
    push %r12
    mov %rcx,%r10
    mov $0x12345678,%r9d
    mov $0x76543210,%r12d
    mov $56,%eax
    syscall
.Lclone_after:
    cmp $.Lclone_after,%rcx
    jne .Lbad
    jmp .Lcheck
.global raw_clone3
raw_clone3:
    push %r12
    mov $0x12345678,%r9d
    mov $0x76543210,%r12d
    mov $435,%eax
    syscall
.Lclone3_after:
    cmp $.Lclone3_after,%rcx
    jne .Lbad
.Lcheck:
    cmp $0x12345678,%r9
    jne .Lbad
    cmp $0x76543210,%r12
    jne .Lbad
    test %rax,%rax
    jz .Lchild
    pop %r12
    ret
.Lchild:
    cmpq $0x33445566,-8(%rsp)
    jne .Lbad
    cmpq $0x55667711,-128(%rsp)
    jne .Lbad
    mov (%rsp),%rdi
    call raw_child
    ud2
.global raw_vfork
raw_vfork:
    push %r12
    mov $0x12345678,%r9d
    mov $0x76543210,%r12d
    mov $58,%eax
    syscall
    cmp $0x12345678,%r9
    jne .Lbad
    cmp $0x76543210,%r12
    jne .Lbad
    test %rax,%rax
    jz 1f
    pop %r12
    ret
1:  call raw_child
.Lbad:
    mov $231,%eax
    mov $199,%edi
    syscall
    ud2
.global raw_unmap_exit
raw_unmap_exit:
    /* This valid kernel sequence must not touch the unmapped stack or TLS. */
    mov $11,%eax
    syscall
    mov $60,%eax
    xor %edi,%edi
    syscall
    ud2
.section .note.GNU-stack,"",@progbits
