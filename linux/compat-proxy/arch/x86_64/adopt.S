/* x86_64 glibc ucontext greg order. No libc/TLS dependencies. */
.text
.global adopt_run
.type adopt_run,@function
adopt_run:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp,24(%rdi)
    mov 16640(%rdi),%rax
    test %rax,%rax
    jz 1f
    mov %rax,%rdx
    shr $32,%rdx
    xrstor64 256(%rdi)
    jmp 2f
1:  fxrstor64 256(%rdi)
2:  lea 56(%rdi),%rdx
    pushq 136(%rdx)
    popfq
    mov 120(%rdx),%rsp
    pushq 128(%rdx)
    mov 0(%rdx),%r8
    mov 8(%rdx),%r9
    mov 16(%rdx),%r10
    mov 24(%rdx),%r11
    mov 32(%rdx),%r12
    mov 40(%rdx),%r13
    mov 48(%rdx),%r14
    mov 56(%rdx),%r15
    mov 64(%rdx),%rdi
    mov 72(%rdx),%rsi
    mov 80(%rdx),%rbp
    mov 88(%rdx),%rbx
    mov 112(%rdx),%rcx
    mov $0,%eax
    mov 96(%rdx),%rdx
    ret
/* Arrive here after sigreturn consumed the application-stack frame. Only
 * now may joiners free that stack. RDI is the resident registry entry. */
.global adopt_exit_return
.type adopt_exit_return,@function
adopt_exit_return:
    sub $8,%rsp
    call adopt_publish_exit
    add $8,%rsp
.global adopt_return
.type adopt_return,@function
adopt_return:
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.section .note.GNU-stack,"",@progbits
