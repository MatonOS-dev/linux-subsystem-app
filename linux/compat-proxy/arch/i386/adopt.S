.text
.global adopt_i386_run
.hidden adopt_i386_run
.type adopt_i386_run,@function
adopt_i386_run:
 push %ebp
 push %ebx
 push %esi
 push %edi
 mov 28(%esp),%ecx
 mov 32(%esp),%eax
 mov 36(%esp),%edx
 test %eax,%eax
 jnz 1f
 test %edx,%edx
 jnz 1f
 fxrstor (%ecx)
 jmp 2f
1: xrstor (%ecx)
2: mov 20(%esp),%edx
 mov 24(%esp),%eax
 mov %esp,(%eax)
 pushl 64(%edx)
 popfl
 mov 28(%edx),%esp
 pushl 56(%edx)
 mov 16(%edx),%edi
 mov 20(%edx),%esi
 mov 24(%edx),%ebp
 mov 32(%edx),%ebx
 mov 40(%edx),%ecx
 mov 36(%edx),%edx
 mov $0,%eax
 ret
/* rt_sigreturn consumes the app's signal frame before pthread cleanup can
 * publish the app clear_tid and let a joiner unmap the app stack. */
.global adopt_i386_return
.hidden adopt_i386_return
.type adopt_i386_return,@function
adopt_i386_return:
 pop %edi
 pop %esi
 pop %ebx
 pop %ebp
 ret
.section .note.GNU-stack,"",@progbits
