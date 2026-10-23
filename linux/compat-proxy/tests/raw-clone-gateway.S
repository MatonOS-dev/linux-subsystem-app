.text
.global raw_clone_thread
.type raw_clone_thread,@function
raw_clone_thread:
    push %r12
    push %r13
    mov %rdx,%r12
    mov %rcx,%r13
    mov %rdi,%rax
    mov %rsi,%rdi
    mov %rax,%rdi
    xor %edx,%edx
    xor %r10d,%r10d
    xor %r8d,%r8d
    mov $56,%eax
    syscall
.global raw_clone_after
raw_clone_after:
    test %rax,%rax
    jz 2f
    pop %r13
    pop %r12
    ret
2:
    mov %r13,%rdi
    call *%r12
    mov %eax,%edi
    mov $60,%eax
    syscall
    ud2
.section .note.GNU-stack,"",@progbits
