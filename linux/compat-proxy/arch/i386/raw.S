.section .proxy_gateway,"ax",@progbits
.global proxy_raw
.type proxy_raw,@function
proxy_raw:
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
.global proxy_raw_after
proxy_raw_after:
 pop %ebx
 pop %esi
 pop %edi
 pop %ebp
 ret
.global proxy_raw_clone_thread
.type proxy_raw_clone_thread,@function
proxy_raw_clone_thread:
 /* (flags, stack_top, entry, arg) */
 push %ebp
 push %edi
 push %esi
 push %ebx
 mov 20(%esp),%ebx
 mov 24(%esp),%ecx
 mov 28(%esp),%ebp
 mov 32(%esp),%edi
 mov $120,%eax
 xor %edx,%edx
 xor %esi,%esi
 int $0x80
.global proxy_raw_clone_after
proxy_raw_clone_after:
 test %eax,%eax
 jnz 1f
 /* i386 SysV ABI: esp must be 16-byte aligned at the call site, so the
  * callee enters with esp%16==12. The clone stack is only page aligned, and
  * -msse2 codegen trusts that promise (movaps to aligned frame slots), so a
  * 4-byte slip kills the helper thread on its first vectorised store. */
 mov %ecx,%esp
 and $-16,%esp
 sub $12,%esp
 push %edi
 call *%ebp
 mov %eax,%ebx
 mov $1,%eax
 int $0x80
 ud2
1: pop %ebx
 pop %esi
 pop %edi
 pop %ebp
 ret
.global proxy_restore
.type proxy_restore,@function
proxy_restore:
 mov $173,%eax
 int $0x80
 ud2
.global proxy_enter
.type proxy_enter,@function
proxy_enter:
 mov 4(%esp),%ecx
 mov 8(%esp),%esp
 xor %edx,%edx
 xor %eax,%eax
 mov %ax,%gs
 jmp *%ecx
.org 0x100
.global proxy_clone_entry
proxy_clone_entry:
 int $0x80
.global proxy_clone_after
proxy_clone_after:
 pushal
 mov %esp,%ebp
 and $-16,%esp
 sub $528,%esp
 fxsave 16(%esp)
 mov 28(%ebp),%eax
 mov %eax,(%esp)
 call dns_fork_result
 mov 28(%ebp),%eax
 mov %eax,(%esp)
 call ipc_fork_result
 /* Mirror CLONE_SETTLS's kernel-installed descriptor into the child's
  * virtual table before control reaches the i386 payload. */
 push 4(%ebp)                 /* original ESI: TLS descriptor pointer */
 push 16(%ebp)                /* original EBX: clone flags */
 call proxy_clone_tls_child
 add $8,%esp
 fxrstor 16(%esp)
 mov %ebp,%esp
 popal
 /* PIC continuation: preserve every payload register and flags. */
 push %eax
 call 2f
2: pop %eax
 lea proxy_clone_resume-2b(%eax),%eax
 mov (%eax),%eax
 xchg %eax,(%esp)
 ret
.weak dns_fork_result
dns_fork_result:
 ret
.section .note.GNU-stack,"",@progbits
