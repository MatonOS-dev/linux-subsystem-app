.text
.global proxy_sigaltstack_on_stack
.type proxy_sigaltstack_on_stack,@function
proxy_sigaltstack_on_stack:
    /* long proxy_sigaltstack_on_stack(stack_t *ss, void *stack_top)
     * Call sigaltstack from a mapping distinct from the currently active
     * alternate stack. Linux returns EPERM if the caller's RSP is still on
     * that stack. Keep the original C frame parked until the raw syscall is
     * complete, then resume it unchanged. */
    push %rbx
    mov %rsp,%rbx
    mov %rsi,%rsp
    and $-16,%rsp
    /* Reuse the exact raw syscall PC approved by both proxy filters.
     * An inline syscall here would TRAP with SIGSYS already blocked. */
    mov %rdi,%rsi
    mov $131,%edi
    xor %edx,%edx
    xor %ecx,%ecx
    xor %r8d,%r8d
    xor %r9d,%r9d
    sub $16,%rsp
    movq $0,(%rsp)
    call proxy_raw
    mov %rbx,%rsp
    pop %rbx
    ret
.size proxy_sigaltstack_on_stack,.-proxy_sigaltstack_on_stack
.section .note.GNU-stack,"",@progbits
