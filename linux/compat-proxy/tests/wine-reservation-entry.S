#if defined(__x86_64__)
.global _start
_start:
    andq $-16, %rsp
    call main
    movl %eax, %edi
    movl $60, %eax
    syscall
#else
.global _start
_start:
    andl $-16, %esp
    call main
    movl %eax, %ebx
    movl $1, %eax
    int $0x80
#endif
.section .note.GNU-stack,"",@progbits
