/* SPDX-License-Identifier: Apache-2.0
 * Address window for the session's 32-bit native (bionic) runtime. Shared by
 * the proxy and the freestanding i386 peer. Kept above 2 GiB: Wine's
 * preloader must own 0x10000-0x68000000 and 0x7f000000-0x7fffffff. */
#ifndef MATON_BIONIC32_RANGE_H
#define MATON_BIONIC32_RANGE_H
#define BIONIC32_SIZE UINT32_C(0x10000000)
#define BIONIC32_FLOOR UINT32_C(0x80000000)
#define BIONIC32_CEIL UINT32_C(0xf0000000)
/* Fixed placement at the top of the window: deterministic logs, and ASLR
 * buys nothing in a 32-bit address space. */
#define BIONIC32_BASE (BIONIC32_CEIL-BIONIC32_SIZE)
#endif
