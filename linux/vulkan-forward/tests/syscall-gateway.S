/* Native C calling convention, with one identifiable raw return PC. */
.text
.global mv_test_gateway
.global mv_test_gateway_after
.type mv_test_gateway,@function
mv_test_gateway:
#ifdef __x86_64__
 mov %rdi,%rax
 mov %rsi,%rdi
 mov %rdx,%rsi
 mov %rcx,%rdx
 mov %r8,%r10
 mov %r9,%r8
 mov 8(%rsp),%r9
 syscall
mv_test_gateway_after:
 ret
#else
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
 int $0x80
mv_test_gateway_after:
 pop %ebx
 pop %esi
 pop %edi
 pop %ebp
 ret
#endif
.section .note.GNU-stack,"",@progbits
