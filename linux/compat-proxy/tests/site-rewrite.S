.text
.macro begin name
.global \name
.type \name,@function
\name:
.endm
.macro end name
.size \name,.-\name
.endm
begin rw_open
mov %rcx,%r10
mov $257,%eax
syscall
ret
end rw_open
begin rw_open9
mov %rcx,%r10
mov $257,%rax
syscall
ret
end rw_open9
begin rw_stat
mov %rcx,%r10
mov $262,%eax
syscall
ret
end rw_stat
begin rw_statx
mov %rcx,%r10
mov $332,%eax
syscall
ret
end rw_statx
begin rw_close
mov $3,%eax
syscall
ret
end rw_close
begin rw_fstat
mov $5,%eax
syscall
ret
end rw_fstat
begin rw_lseek
mov $8,%eax
syscall
ret
end rw_lseek
begin rw_readlink
mov $89,%eax
syscall
ret
end rw_readlink
begin rw_readlinkat
mov %rcx,%r10
mov $267,%eax
syscall
ret
end rw_readlinkat
begin rw_getdents
mov $217,%eax
syscall
ret
end rw_getdents
begin rw_fcntl
mov $72,%eax
syscall
ret
end rw_fcntl
begin rw_preserve
push %rbx
push %rbp
push %r12
mov %rdi,%rbx
mov %rsi,%rbp
mov $99,%r12
mov %rbx,%rsi
mov %rbp,%rdx
mov $-100,%rdi
xor %ecx,%ecx
mov $42,%r8
mov $43,%r9
movq %r8,%xmm0
stc
call rw_stat
jnc 9f
test %rax,%rax
jnz 9f
cmp $-100,%rdi
jne 9f
cmp %rbx,%rsi
jne 9f
cmp %rbp,%rdx
jne 9f
cmp $42,%r8
jne 9f
cmp $43,%r9
jne 9f
test %r10,%r10
jnz 9f
cmp $99,%r12
jne 9f
movq %xmm0,%rax
cmp $42,%rax
jne 9f
xor %eax,%eax
jmp 8f
9: mov $1,%eax
8: pop %r12
pop %rbp
pop %rbx
ret
end rw_preserve
begin rw_preserve_link
push %rbx
push %rbp
push %r12
mov %rdi,%rbx
mov %rsi,%rbp
mov %rdx,%r12
mov $52,%r10
mov $53,%r8
mov $54,%r9
call rw_readlink
test %rax,%rax
jle 9f
cmp %rbx,%rdi
jne 9f
cmp %rbp,%rsi
jne 9f
cmp %r12,%rdx
jne 9f
cmp $52,%r10
jne 9f
cmp $53,%r8
jne 9f
cmp $54,%r9
jne 9f
xor %eax,%eax
jmp 8f
9: mov $1,%eax
8: pop %r12
pop %rbp
pop %rbx
ret
end rw_preserve_link
begin rw_overlap_outer
nop
begin rw_overlap_inner
mov $257,%eax
syscall
ret
end rw_overlap_inner
end rw_overlap_outer
begin rw_branch
test %edi,%edi
jz 1f
mov $257,%eax
1: syscall
ret
end rw_branch
begin rw_redzone
mov %rdi,-8(%rsp)
mov $257,%eax
syscall
ret
end rw_redzone
/* Put the 7-byte site across a page boundary. */
.p2align 12
.fill 4093,1,0x90
begin rw_straddle
mov $257,%eax
syscall
ret
end rw_straddle
.section .note.GNU-stack,"",@progbits
