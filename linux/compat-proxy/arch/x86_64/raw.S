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
    mov $158,%rax
    mov $0x1002,%rdi
    xor %rsi,%rsi
    syscall
    mov %r13,%rsp
    xor %rdx,%rdx
    jmp *%r12
/* Kernel-only vfork retry: RCX carries the syscall-clobbered return PC;
 * R11 carries the kernel-decoded child SP before syscall. Reserve scratch
 * outside the x86_64 ABI red zone, without knowing any libc stack layout.
 * Parent recovers its continuation by raw TID (shared-stack child may have
 * overwritten scratch); child touches only its own app stack, never records.
 * Every non-syscall-clobbered register, including R9, is preserved. */
.org 0x80
.global adopt_vfork_entry
.type adopt_vfork_entry,@function
adopt_vfork_entry:
    mov %rcx,-136(%r11)
    syscall
    jmp adopt_vfork_finish
/* Ordinary syscalls run after sigreturn, so blocking waits retain native
 * signal interruption/restart semantics. Reserve space below the red zone
 * only after the old signal frame has been consumed. RCX is syscall-clobbered. */
.org 0xa0
.global adopt_syscall_entry
.type adopt_syscall_entry,@function
adopt_syscall_entry:
    lea -256(%rsp),%rsp
    mov %rcx,120(%rsp)
    syscall
    mov 120(%rsp),%rcx
    lea 256(%rsp),%rsp
    jmp *%rcx
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
    cmpq $0,72(%rbp)
    jge 1f
    mov $-1,%rdi
    call links_fork
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
.text
.global adopt_vfork_finish
.type adopt_vfork_finish,@function
adopt_vfork_finish:
    lea -256(%rsp),%rsp
    pushfq
    test %rax,%rax
    jz 9f
    push %rax
    push %rdi
    push %rsi
    push %rdx
    push %r8
    push %r9
    push %r10
    push %r11
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp,%r12
    and $-16,%rsp
    call adopt_vfork_parent
    mov %r12,%rsp
    mov %rax,%rcx
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rax
    popfq
    lea 256(%rsp),%rsp
    jmp *%rcx
9:
    popfq
    lea 256(%rsp),%rsp
    mov -136(%rsp),%rcx
    jmp *%rcx
/* Normal freestanding proxy builds do not use the adoption retry. */
.weak adopt_vfork_parent
.type adopt_vfork_parent,@function
adopt_vfork_parent:
    ud2
.section .note.GNU-stack,"",@progbits
