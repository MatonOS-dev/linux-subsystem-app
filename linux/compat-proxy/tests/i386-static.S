/* SPDX-License-Identifier: Apache-2.0
 * Original ELF32 P1 fixture: no libc, runtime, TLS or copied implementation.
 * argv[1] is a pathname; stat64/open/read/write must all reach that file.
 */
.section .text
.global _start
_start:
#ifdef I386_EXIT_ONLY
    xor %ebx, %ebx
    mov $1, %eax
    int $0x80
    ud2
#endif
    cmpl $2, (%esp)
    jne fail
    mov 8(%esp), %ebx
    mov $195, %eax                 /* stat64(path, buf) */
    mov $status, %ecx
    int $0x80
    test %eax, %eax
    jne fail
    mov $5, %eax                   /* open(path, O_RDONLY, 0) */
    xor %ecx, %ecx
    xor %edx, %edx
    int $0x80
    test %eax, %eax
    js fail
    mov %eax, %ebx
    mov $3, %eax                   /* read(fd, buf, 32) */
    mov $buffer, %ecx
    mov $32, %edx
    int $0x80
    cmp $13, %eax
    jne fail
    mov $buffer, %esi
    mov $expected, %edi
    mov $13, %ecx
    cld
    repe cmpsb
    jne fail
    mov $6, %eax                   /* close */
    int $0x80
    test %eax, %eax
    jne fail
    mov $4, %eax                   /* write(1, buf, 13) */
    mov $1, %ebx
    mov $buffer, %ecx
    mov $13, %edx
    int $0x80
    cmp $13, %eax
    jne fail
    xor %ebx, %ebx
    jmp finish
fail:
    mov $1, %ebx
finish:
    mov $1, %eax                   /* exit */
    int $0x80
    ud2
.section .rodata
expected: .ascii "i386-p1-ok\n!!"
.section .bss
.balign 16
status: .skip 128
buffer: .skip 32
.section .note.GNU-stack,"",@progbits
