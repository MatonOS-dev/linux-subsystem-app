.text
.global i386_vsyscall6
.type i386_vsyscall6,@function
i386_vsyscall6:
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
    call *%gs:0x10
    pop %ebx
    pop %esi
    pop %edi
    pop %ebp
    ret
.size i386_vsyscall6,.-i386_vsyscall6

.global i386_check_sysinfo_preserve
.type i386_check_sysinfo_preserve,@function
i386_check_sysinfo_preserve:
    push %ebp
    push %edi
    push %esi
    push %ebx
    mov 20(%esp),%ebx
    mov $0x111,%ecx
    mov $0x222,%edx
    mov $0x333,%esi
    mov $0x444,%edi
    mov $0x555,%ebp
    mov $6,%eax
    stc
    call *%gs:0x10
    pushfl
    test %eax,%eax
    jne .Lpreserve_fail
    cmpl $0x111,%ecx
    jne .Lpreserve_fail
    cmpl $0x222,%edx
    jne .Lpreserve_fail
    cmpl $0x333,%esi
    jne .Lpreserve_fail
    cmpl $0x444,%edi
    jne .Lpreserve_fail
    cmpl $0x555,%ebp
    jne .Lpreserve_fail
    cmpl 24(%esp),%ebx            /* saved caller fd */
    jne .Lpreserve_fail
    testl $1,(%esp)               /* carry flag survives direct entry */
    jz .Lpreserve_fail
    xor %eax,%eax
    jmp .Lpreserve_done
.Lpreserve_fail:
    mov $1,%eax
.Lpreserve_done:
    add $4,%esp
    pop %ebx
    pop %esi
    pop %edi
    pop %ebp
    ret
.size i386_check_sysinfo_preserve,.-i386_check_sysinfo_preserve
.section .note.GNU-stack,"",@progbits
