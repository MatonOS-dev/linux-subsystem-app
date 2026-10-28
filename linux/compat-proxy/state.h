/* SPDX-License-Identifier: Apache-2.0
 * Private exec wire stream. No native pointers/longs cross this boundary.
 * All lengths/counts are bounded; errors latch and abort the handoff. */
#ifndef V3_STATE_H
#define V3_STATE_H
#include <stdint.h>
#include <stddef.h>
struct state { int fd,reading,error; uint64_t pos,size; const unsigned char *data; };
void state_bytes(struct state *,void *,size_t);
uint64_t state_word(struct state *,uint64_t);
uint64_t state_count(struct state *,uint64_t);
void state_string(struct state *,char **);
int state_live(int);
int state_private_fd(int);
void proxy_fd_state(struct state *);
long proxy_private_fd_limit(uint64_t);
int state_exec(const char *,char *const *,char *const *);
extern int state_android, state_gpu_bridge;
extern char **state_environment;
int state_import(const char *,const char *);
void vfs_state(struct state *);
void ipc_state(struct state *);
void net_state(struct state *);
void proxy_state(struct state *);
#endif
