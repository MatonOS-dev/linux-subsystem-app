/* Actual ELF32 exercises both sealed handoffs with an inherited slice. */
.global _start
.text
_start:
 mov $19,%eax
 mov $90,%ebx
 xor %ecx,%ecx
 mov $1,%edx
 int $0x80
 cmp $5,%eax
 jne fail
 mov $3,%eax
 mov $90,%ebx
 mov $buffer,%ecx
 mov $1,%edx
 int $0x80
 cmp $1,%eax
 jne fail
 cmpb $'5',buffer
 jne fail
 mov $197,%eax
 mov $90,%ebx
 mov $metadata,%ecx
 int $0x80
 test %eax,%eax
 jne fail
 cmpl $16,metadata+44
 jne fail
 mov $55,%eax
 mov $91,%ebx
 mov $1,%ecx
 int $0x80
 cmp $-9,%eax
 jne fail
 mov $55,%eax
 mov $92,%ebx
 mov $1,%ecx
 int $0x80
 cmp $-9,%eax
 jne fail
 mov $5,%eax
 mov $data,%ebx
 xor %ecx,%ecx
 int $0x80
 test %eax,%eax
 js fail
 mov %eax,%ebx
 mov $3,%eax
 mov $buffer,%ecx
 mov $32,%edx
 int $0x80
 cmp $16,%eax
 jne fail
 mov $6,%eax
 int $0x80
 mov $19,%eax
 mov $90,%ebx
 mov $5,%ecx
 xor %edx,%edx
 int $0x80
 cmp $5,%eax
 jne fail
 mov $11,%eax
 mov $probe,%ebx
 mov $args,%ecx
 xor %edx,%edx
 int $0x80
fail:
 mov $1,%eax
 mov $99,%ebx
 int $0x80
.data
probe: .asciz "/packed/probe"
data: .asciz "/packed/data"
number: .asciz "90"
.align 4
args: .long probe,number,0
.bss
.align 8
buffer: .skip 32
metadata: .skip 96
.section .note.GNU-stack,"",@progbits
