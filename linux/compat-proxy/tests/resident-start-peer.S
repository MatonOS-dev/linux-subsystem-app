/* Model the Android peer linker's mmap2 before SIGSYS/state import exists.
 * Deliberately outside the resident raw gateway. */
.global _start
.text
_start:
 mov %esp,%edi
 mov $192,%eax
 xor %ebx,%ebx
 mov $4096,%ecx
 mov $3,%edx
 mov $0x22,%esi
 push %edi
 mov $-1,%edi
 xor %ebp,%ebp
 int $0x80
 pop %edi
 cmp $-4095,%eax
 jae fail
 mov (%edi),%eax
 lea 8(%edi,%eax,4),%edx
 lea 4(%edi),%ecx
 mov $real_peer,%ebx
 mov $11,%eax
 int $0x80
fail:
 mov $1,%eax
 mov $85,%ebx
 int $0x80
.section .rodata
real_peer: .asciz REAL_PEER
.section .note.GNU-stack,"",@progbits
