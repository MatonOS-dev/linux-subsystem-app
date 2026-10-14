/* Test entry uses glibc _exit so stdio is not silently lost. Production
 * freestanding entry instead issues exit_group directly. */
.text
.global _start
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
 mov %eax,(%esp)
 call exit
 ud2
.section .note.GNU-stack,"",@progbits
