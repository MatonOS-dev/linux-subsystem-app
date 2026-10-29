.text
.global terminal_vsyscall
.type terminal_vsyscall,@function
terminal_vsyscall:
    push %ebp
    push %edi
    push %esi
    push %ebx
    mov 20(%esp),%eax
    mov 24(%esp),%ebx
    mov 28(%esp),%ecx
    mov 32(%esp),%edx
    mov 36(%esp),%esi
    mov 40(%esp),%edi
    mov 44(%esp),%ebp
    call *terminal_sysinfo
    pop %ebx
    pop %esi
    pop %edi
    pop %ebp
    ret
.size terminal_vsyscall,.-terminal_vsyscall

.section .note.GNU-stack,"",@progbits
