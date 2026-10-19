.section .proxy_gateway,"ax",@progbits
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
    .org 0x40,0x90
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
    mov %r13,%rsp
    xor %rdx,%rdx
    jmp *%r12
/* Retry clone on the application's stack, retaining its continuation even
 * when the kernel switches to a posix_spawn child stack. */
.org 0x100
.global proxy_clone_entry
.type proxy_clone_entry,@function
proxy_clone_entry:
    syscall
.global proxy_clone_after
proxy_clone_after:
    /* Complete IPC inheritance before either continuation reaches app code.
     * Preserve syscall registers and the compiler's SSE/x87 scratch state. */
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
    sub $512,%rsp
    fxsave64 (%rsp)
    mov 72(%rbp),%rdi
    call dns_fork_result
    mov 72(%rbp),%rdi
    call ipc_fork_result
    /* Adoption's child-side maps point at vanished threads after a process
     * clone. Rebase them before returning to app code; standalone proxy builds
     * and tests use the weak no-op below. */
    /* Saved syscall result is at +72: +0 is the caller's saved RBP. */
    cmpq $0,72(%rbp)
    jne 1f
    call adopt_fork_child
1:
    fxrstor64 (%rsp)
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
    jmp *proxy_clone_resume(%rip)
/* raw.S is also linked by standalone IPC tests with no proxy DNS state. */
.weak dns_fork_result
.type dns_fork_result,@function
dns_fork_result:
    ret
.weak adopt_fork_child
.type adopt_fork_child,@function
adopt_fork_child:
    ret
.section .note.GNU-stack,"",@progbits
