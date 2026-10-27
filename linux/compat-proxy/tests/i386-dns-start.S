/* Exercise the real helper startup through i386 socketcall. No DNS service
 * is needed: connecting UDP starts the helper but does not send a query. */
.section .text
.global _start
_start:
 mov $102,%eax
 mov $1,%ebx
 mov $socket_args,%ecx
 int $0x80
 test %eax,%eax
 js fail
 mov %eax,connect_args
 mov $102,%eax
 mov $3,%ebx
 mov $connect_args,%ecx
 int $0x80
 test %eax,%eax
 jnz fail
 xor %ebx,%ebx
 jmp exit
fail:
 mov $1,%ebx
exit:
 mov $252,%eax
 int $0x80
.section .data
socket_args: .long 2,2,0
connect_args: .long 0,resolver,16
resolver: .byte 2,0,0,53,127,0,0,53
 .zero 8
.section .note.GNU-stack,"",@progbits
