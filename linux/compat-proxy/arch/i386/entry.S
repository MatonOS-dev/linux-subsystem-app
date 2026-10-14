.text
.global _start
.type _start,@function
_start:
 xor %ebp,%ebp
 mov %esp,%eax
 and $-16,%esp
 sub $4,%esp
 mov (%eax),%ecx
 lea 8(%eax,%ecx,4),%edx
 push %edx
 lea 4(%eax),%edx
 push %edx
 push %ecx
 call main
 mov %eax,%ebx
 mov $252,%eax
 int $0x80
 ud2
.section .note.GNU-stack,"",@progbits
