/* Build-selected architecture boundary. Internal IDs must be encoded before
 * kernel entry or BPF comparisons. Context slots expose payload argument,
 * result, PC and stack words; only the backend knows REG_* indices.
 * Wire adapters in arch/{stat,socket,time,ipc}.h normalize the supported ABI
 * variants; legacy variants still listed in README require additional hooks.
 * ARCH_FILTER_HEADER owns inherited multi-ABI BPF/gateways; exec.h owns
 * validated peer class selection. Core state streams never serialize pointers. */
#ifndef PROXY_ARCH_H
#define PROXY_ARCH_H
#include <stdint.h>
#include "syscalls.h"
/* Six signed 64-bit canonical words. Pointers enter through uintptr_t;
 * backends sign-extend scalar compat arguments during decode. */
typedef int64_t proxy_arg_t;
typedef proxy_arg_t proxy_args[6];
typedef uint64_t proxy_ptr_t;
typedef uint64_t proxy_sigmask_t;
enum arch_slot { ARCH_RESULT, ARCH_PC, ARCH_STACK, ARCH_ARG0, ARCH_ARG1,
                 ARCH_ARG2, ARCH_ARG3, ARCH_ARG4, ARCH_ARG5 };
#ifndef PROXY_ARCH_BACKEND
#define PROXY_ARCH_BACKEND "x86_64/backend.h"
#endif
/* Decode retains any wire variant until arch_finish_call fixes its result.
 * Multiplexer tables and user-buffer layouts belong entirely in the backend. */
struct arch_call { long id,error; unsigned variant; uintptr_t output; };
long proxy_copy(void *,const void *,__SIZE_TYPE__);
long proxy_raw(long,long,long,long,long,long,long);
/* Backends declare ARCH_ELF_CLASS/MACHINE for this loader and
 * ARCH_PEER_ELF_CLASS/MACHINE next to ARCH_PEER_LOADER for its supported peer.
 * ELF record parsing stays in core; supported class/machine pairs live here. */
#include PROXY_ARCH_BACKEND
#define arch_encode_syscall arch_peer_encode_syscall
#define arch_decode_syscall arch_peer_decode_syscall
#include ARCH_PEER_SYSCALLS
#undef arch_encode_syscall
#undef arch_decode_syscall
#endif
