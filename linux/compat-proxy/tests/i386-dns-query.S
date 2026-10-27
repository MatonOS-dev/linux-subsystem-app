/* Exercise the full i386 DNS helper path through socketcall: connect() starts
 * the helper, sendto() hands it a datagram, and the helper must survive
 * answering it. No dnsproxyd is reachable, so the expected answer is a
 * timeout and EAGAIN, never a process death.
 *
 * This is the regression for the helper thread's stack alignment: the clone
 * entry used to enter dns_worker with esp%16==8 instead of the i386 SysV
 * value 12, and -msse2 codegen then faulted on a movaps to an aligned frame
 * slot while all signals were still blocked, so the whole payload died of an
 * unreported SIGSEGV right after the query was sent. */
.section .text
.global _start
_start:
 /* fd = socket(AF_INET, SOCK_DGRAM|SOCK_NONBLOCK, 0) */
 mov $102,%eax
 mov $1,%ebx
 mov $socket_args,%ecx
 int $0x80
 test %eax,%eax
 js fail_socket
 mov %eax,connect_args
 mov %eax,sendto_args
 mov %eax,pollfd
 mov %eax,recvfrom_args

 /* connect(fd, resolver, 16) starts the helper */
 mov $102,%eax
 mov $3,%ebx
 mov $connect_args,%ecx
 int $0x80
 test %eax,%eax
 jnz fail_connect

 /* sendto(fd, query, 47, 0, resolver, 16) */
 mov $102,%eax
 mov $11,%ebx
 mov $sendto_args,%ecx
 int $0x80
 cmp $47,%eax
 jne fail_sendto

 /* poll(&pollfd, 1, 2000): the helper answers (or gives up) meanwhile */
 mov $168,%eax
 mov $pollfd,%ebx
 mov $1,%ecx
 mov $2000,%edx
 int $0x80
 test %eax,%eax
 js fail_poll

 /* recvfrom(fd, buffer, 512, 0, 0, 0) must report the timeout, not data */
 mov $102,%eax
 mov $12,%ebx
 mov $recvfrom_args,%ecx
 int $0x80
 cmp $-11,%eax
 jne fail_recvfrom

 xor %ebx,%ebx
 jmp emit
fail_socket:
 mov $1,%ebx
 jmp emit
fail_connect:
 mov $2,%ebx
 jmp emit
fail_sendto:
 mov $3,%ebx
 jmp emit
fail_poll:
 mov $4,%ebx
 jmp emit
fail_recvfrom:
 mov $5,%ebx
emit:
 mov %ebx,exit_code
 mov $4,%eax
 mov $1,%ebx
 mov $marker,%ecx
 mov $3,%edx
 int $0x80
 mov exit_code,%ebx
 mov $252,%eax
 int $0x80
.section .data
socket_args: .long 2,0x802,0
connect_args: .long 0,resolver,16
sendto_args: .long 0,query,47,0,resolver,16
recvfrom_args: .long 0,buffer,512,0,0,0
pollfd: .long 0
 .short 1
 .short 0
resolver: .byte 2,0,0,53,127,0,0,53
 .zero 8
/* A minimal A-record query for example.com; the helper does not parse it. */
query: .byte 0x12,0x34,0x01,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00
 .byte 7,0x65,0x78,0x61,0x6d,0x70,0x6c,0x65,3,0x63,0x6f,0x6d,0x00
 .byte 0x00,0x01,0x00,0x01
 .zero 18
buffer: .zero 512
marker: .ascii "ok\n"
exit_code: .long 0
.section .note.GNU-stack,"",@progbits
