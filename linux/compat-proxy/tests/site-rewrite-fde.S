.text
.hidden site_fde_target
.type site_fde_target,@function
site_fde_target:
.cfi_startproc
    mov $3,%eax
    syscall
    ret
.cfi_endproc
.size site_fde_target,.-site_fde_target

.globl site_fde_call
.type site_fde_call,@function
site_fde_call:
.cfi_startproc
    mov $-1,%edi
    jmp site_fde_target
.cfi_endproc
.size site_fde_call,.-site_fde_call

.globl site_fde_address
.type site_fde_address,@function
site_fde_address:
.cfi_startproc
    lea site_fde_target(%rip),%rax
    ret
.cfi_endproc
.size site_fde_address,.-site_fde_address
.section .note.GNU-stack,"",@progbits
