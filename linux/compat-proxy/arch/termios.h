/* x86 classic and extended tty wire formats, identical on both x86 ABIs.
 * A new backend must supply its own sizes and ioctl numbers. */
#ifndef PROXY_ARCH_TERMIOS_H
#define PROXY_ARCH_TERMIOS_H
struct proxy_termios2 {uint32_t c_iflag,c_oflag,c_cflag,c_lflag;
 uint8_t c_line,c_cc[19];uint32_t c_ispeed,c_ospeed;};
_Static_assert(sizeof(struct proxy_termios2)==44,"termios2 wire ABI");
#define V3_TCGETS2 0x802c542aul
#define V3_TCSETS2 0x402c542bul
#define V3_TCSETSW2 0x402c542cul
#define V3_TCSETSF2 0x402c542dul
#endif
