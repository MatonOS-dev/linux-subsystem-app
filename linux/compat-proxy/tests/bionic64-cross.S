/* Preserve env/argv across an actual i386 -> x86_64 kernel exec. */
.text
.global _start
_start:
 mov (%esp),%eax
 lea 8(%esp,%eax,4),%edx
 lea 8(%esp),%ecx
 mov (%ecx),%ebx
 mov $11,%eax
 int $0x80
 mov $111,%ebx
 mov $252,%eax
 int $0x80
.section .note.GNU-stack,"",@progbits
