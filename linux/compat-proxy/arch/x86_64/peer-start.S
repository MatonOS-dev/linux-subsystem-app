/* SPDX-License-Identifier: Apache-2.0 */
.text
.global _start
.hidden peer_start
_start:
 mov %rsp,%rdi
 and $-16,%rsp
 call peer_start
 ud2
.global peer_enter
peer_enter:
 mov %rsi,%rsp
 xor %edx,%edx
 jmp *%rdi
.section .proxy_gateway,"ax",@progbits
.global peer_raw
peer_raw:
 mov %rdi,%rax
 mov %rsi,%rdi
 mov %rdx,%rsi
 mov %rcx,%rdx
 mov %r8,%r10
 mov %r9,%r8
 mov 8(%rsp),%r9
 .org 0x40,0x90
 syscall
 ret
.section .note.GNU-stack,"",@progbits
