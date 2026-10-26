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
    mov $131,%eax
    xor %esi,%esi
    xor %edx,%edx
    xor %r10d,%r10d
    syscall
    mov %rbx,%rsp
    pop %rbx
    ret
.size proxy_sigaltstack_on_stack,.-proxy_sigaltstack_on_stack
.section .note.GNU-stack,"",@progbits
