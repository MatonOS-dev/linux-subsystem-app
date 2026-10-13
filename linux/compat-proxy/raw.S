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
/* Retry clone on the application's stack, retaining its continuation even
 * when the kernel switches to a posix_spawn child stack. */
.global proxy_clone_entry
.type proxy_clone_entry,@function
proxy_clone_entry:
    syscall
.global proxy_clone_after
proxy_clone_after:
    test %rax,%rax
    jns 1f
    /* Failed fork: undo reserved references; preserve the app red zone. */
    lea -128(%rsp),%rsp
    push %rax
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %rbp
    mov %rsp,%rbp
    and $-16,%rsp
    mov $-1,%rdi
    call links_fork
    mov %rbp,%rsp
    pop %rbp
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rax
    lea 128(%rsp),%rsp
1:
    jmp *proxy_clone_resume(%rip)
.section .note.GNU-stack,"",@progbits
