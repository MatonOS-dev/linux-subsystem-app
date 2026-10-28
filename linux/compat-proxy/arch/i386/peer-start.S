/* SPDX-License-Identifier: Apache-2.0 */
.text
.global _start
.hidden peer_start
_start:
 mov %esp,%eax
 and $-16,%esp
 sub $12,%esp
 push %eax
 call peer_start
 ud2
.global peer_enter
peer_enter:
 mov 4(%esp),%eax
 mov 8(%esp),%esp
 xor %edx,%edx
 jmp *%eax
.section .proxy_gateway,"ax",@progbits
.global peer_raw
peer_raw:
 push %ebp
 push %edi
 push %esi
 push %ebx
 mov 20(%esp),%eax
 mov 24(%esp),%ebx
 mov 28(%esp),%ecx
 mov 32(%esp),%edx
 mov 36(%esp),%esi
 mov 40(%esp),%edi
 mov 44(%esp),%ebp
 .org 0x40,0x90
 int $0x80
 pop %ebx
 pop %esi
 pop %edi
 pop %ebp
 ret
/* bionic's AT_SYSINFO entry uses a register-ABI call, not peer_raw's C ABI. */
.org 0x100
 int $0x80
 ret
.section .note.GNU-stack,"",@progbits
