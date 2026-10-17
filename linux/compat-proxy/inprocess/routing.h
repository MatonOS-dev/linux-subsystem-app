/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MATON_INPROCESS_ROUTING_H
#define MATON_INPROCESS_ROUTING_H
#include <signal.h>
#include "session.h"
extern struct resident_session *inprocess_session;
#include <stdint.h>
long inprocess_filter(void);
int inprocess_signals(void (*handler)(int,siginfo_t *,void *));
void inprocess_chain(int,siginfo_t *,void *);
void inprocess_enter(uintptr_t,uintptr_t);
extern int inprocess_status;
extern int inprocess_output[2];
int inprocess_run_fds(char *const argv[],char *const env[],const int fds[2]);
int inprocess_run(char *const argv[],char *const env[]);
#endif
