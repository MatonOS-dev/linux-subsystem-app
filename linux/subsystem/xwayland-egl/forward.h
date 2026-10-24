#pragma once
/* The only architecture-specific code of the forwarding shims: each
 * MATON_FORWARD(name, i) defines an exported function `name` that
 * tail-jumps through maton_forward_table[i], leaving the caller's arguments
 * untouched, so no prototype is needed (variadic functions included). */
#define MATON_STR_(x) #x
#define MATON_STR(x) MATON_STR_(x)

#if defined(__x86_64__)
#define MATON_FORWARD(name, i) __asm__(                                       \
    ".globl " #name "\n.type " #name ",@function\n" #name ":\n"               \
    "  jmp *maton_forward_table+" MATON_STR(i) "*8(%rip)\n");
#elif defined(__aarch64__)
/* x16 is the AAPCS64 intra-procedure-call scratch register for veneers. */
#define MATON_FORWARD(name, i) __asm__(                                       \
    ".globl " #name "\n.type " #name ",%function\n" #name ":\n"               \
    "  adrp x16, maton_forward_table\n"                                        \
    "  add  x16, x16, :lo12:maton_forward_table\n"                             \
    "  ldr  x16, [x16, #" MATON_STR(i) "*8]\n"                                 \
    "  br   x16\n");
#else
#error "MATON_FORWARD: add a tail-jump for this architecture"
#endif
