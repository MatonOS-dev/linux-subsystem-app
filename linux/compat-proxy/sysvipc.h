#include "arch/arch.h"
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PROXY_SYSVIPC_H
#define PROXY_SYSVIPC_H
int ipc_init(void);
int ipc_ours(long);
long ipc_call(long,proxy_arg_t *);
void ipc_exit(void);
void ipc_thread_exit(void);
void ipc_exec(void);
void ipc_fork(void);
int ipc_fork_prepare(void);
void ipc_fork_result(long);
int ipc_private_fd(int);
void ipc_private_fd_relocated(int,int);
#endif
