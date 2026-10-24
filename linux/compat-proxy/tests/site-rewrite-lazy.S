.text
.globl lazy_open
.type lazy_open,@function
lazy_open:
    mov $257,%eax
    mov %rcx,%r10
.globl lazy_syscall
lazy_syscall:
    syscall
    ret
.size lazy_open,.-lazy_open
/* Test hook: retain the state after the number load while another thread
 * publishes the real wrapper. The signal handler redirects to lazy_syscall. */
.globl lazy_park
.type lazy_park,@function
lazy_park:
    mov %rcx,%r10
    mov $257,%eax
    movl $1,lazy_phase(%rip)
1:  pause
    jmp 1b
.size lazy_park,.-lazy_park
.globl lazy_clobber
.type lazy_clobber,@function
lazy_clobber:
    mov $257,%eax
    xor %eax,%eax
    syscall
    ret
.size lazy_clobber,.-lazy_clobber
.globl lazy_read
.type lazy_read,@function
lazy_read:
    mov $257,%eax
    mov %eax,%r11d
    syscall
    ret
.size lazy_read,.-lazy_read
.section .note.GNU-stack,"",@progbits
