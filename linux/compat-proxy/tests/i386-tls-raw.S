/* Freestanding direct-syscall coverage for the TLS descriptor path. */
.section .text
.global _start
_start:
    mov $243,%eax
    mov $desc,%ebx
    int $0x80
    test %eax,%eax
    jne fail
    mov %gs,%cx
    mov desc,%ax
    shl $3,%ax
    or $3,%ax
    mov %ax,%gs
    mov %gs:0,%eax
    cmp value,%eax
    jne fail_restore
    mov %cx,%gs
    mov desc,%eax
    mov %eax,desc_out
    mov $244,%eax
    mov $desc_out,%ebx
    int $0x80
    test %eax,%eax
    jne fail
    mov desc_out+4,%eax
    cmp $value,%eax
    jne fail
    mov $1,%eax
    xor %ebx,%ebx
    int $0x80
fail_restore:
    mov %cx,%gs
fail:
    mov $1,%eax
    mov $1,%ebx
    int $0x80
.section .data
.align 4
desc:
    .long -1
    .long value
    .long 0xfffff
    .long 0x51
desc_out:
    .zero 16
value:
    .long 0x51a7c0de
