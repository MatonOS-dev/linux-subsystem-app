.text
.global proxy_raw
.type proxy_raw,@function
proxy_raw:
    mov %rdi,%rax
    mov %rsi,%rdi
    mov %rdx,%rsi
    mov %rcx,%rdx
    mov %r8,%r10
    mov %r9,%r8
    mov 8(%rsp),%r9
    syscall
.global proxy_raw_after
proxy_raw_after:
    ret
.global proxy_restore
.type proxy_restore,@function
proxy_restore:
    mov $15,%rax
    syscall
    ud2
.global proxy_enter
.type proxy_enter,@function
proxy_enter:
    mov %rdi,%r12
    mov %rsi,%r13
    mov $158,%rax
    mov $0x1002,%rdi
    xor %rsi,%rsi
    syscall
    mov %r13,%rsp
    xor %rdx,%rdx
    jmp *%r12
.section .note.GNU-stack,"",@progbits
