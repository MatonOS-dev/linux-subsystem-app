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
.global proxy_raw_clone_thread
.type proxy_raw_clone_thread,@function
proxy_raw_clone_thread:
    /* (flags, stack_top, entry, arg); child starts on the supplied stack. */
    push %r12
    push %r13
    mov %rdx,%r12
    mov %rcx,%r13
    mov %rdi,%rax
    mov %rsi,%rdi
    mov %rsi,%rsi
    mov %rsi,%rsi
    /* clone(flags, child_stack, parent_tid, child_tid, tls) */
    mov %rax,%rdi
    mov %rsi,%rsi
    xor %edx,%edx
    xor %r10d,%r10d
    xor %r8d,%r8d
    mov $56,%eax
    syscall
.global proxy_raw_clone_after
proxy_raw_clone_after:
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
    mov 40(%rbp),%rsi
    call proxy_trace_clone_result
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
    call proxy_thread_state_invalidate
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
.weak adopt_fork_child
.type adopt_fork_child,@function
adopt_fork_child:
    ret
.weak proxy_thread_state_invalidate
.type proxy_thread_state_invalidate,@function
proxy_thread_state_invalidate:
    ret
.weak dns_fork_result
.type dns_fork_result,@function
dns_fork_result:
    ret

/* Rewritten syscall sites call a per-number slot in the low table. RAX
 * identifies the slot; syscall argument registers remain intact. */
/* Standalone RAW-only tests do not link the loader/dispatcher. These
 * references are unused there; full proxy links provide strong definitions. */
.weak proxy_rewrite_table
.weak proxy_direct_dispatch
.hidden proxy_rewrite_table
.hidden proxy_direct_dispatch
.text
.global proxy_rewrite_entry_common
.type proxy_rewrite_entry_common,@function
proxy_rewrite_entry_common:
    pushfq
    lea proxy_rewrite_table(%rip),%r11
    mov (%r11),%r11
    sub %r11,%rax
    shr $5,%rax
    push %r12
    push %r9
    push %r8
    push %r10
    push %rdx
    push %rsi
    push %rdi
    mov %rsp,%r12
    mov %rsp,%rsi
    mov %eax,%edi
    and $-16,%rsp
    sub $512,%rsp
    fxsave64 (%rsp)
    call proxy_direct_dispatch
    fxrstor64 (%rsp)
    mov %r12,%rsp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %r10
    pop %r8
    pop %r9
    pop %r12
    popfq
    ret
/* Preserve the million-descriptor spawn sweep's native F_GETFD/F_SETFD
 * bypass. These exact commands already pass the proxy filter; avoid FXSAVE,
 * C dispatch and allocation scopes. Other commands use the regular entry.
 * An app filter's TRAP here is explicitly chained by the SIGSYS demux. */
.global proxy_rewrite_fcntl_entry
.type proxy_rewrite_fcntl_entry,@function
proxy_rewrite_fcntl_entry:
    pushfq
    cmp $1,%rsi
    je 1f
    cmp $2,%rsi
    je 1f
    popfq
    jmp proxy_rewrite_entry_common
1:  popfq
    mov $72,%eax
    syscall
.global proxy_rewrite_fcntl_after
proxy_rewrite_fcntl_after:
    ret
/* Deliberately outside .proxy_gateway: locking/unknown fcntl commands
 * retain the original SIGSYS dispatch and restart behavior. */
.global proxy_rewrite_trap
.type proxy_rewrite_trap,@function
proxy_rewrite_trap:
    mov %rdi,%rax
    mov %rsi,%r11
    mov 0(%r11),%rdi
    mov 8(%r11),%rsi
    mov 16(%r11),%rdx
    mov 24(%r11),%r10
    mov 32(%r11),%r8
    mov 40(%r11),%r9
    syscall
    ret
.weak proxy_trace_clone_result
proxy_trace_clone_result:
    ret
.section .note.GNU-stack,"",@progbits
