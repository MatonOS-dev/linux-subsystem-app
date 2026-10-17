/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MATON_RESIDENT_SESSION_H
#define MATON_RESIDENT_SESSION_H
#include <stdint.h>
struct resident_session;
struct resident_session *resident_session_begin(const int[2]);
/* Trusted setup before payload entry. Payloads cannot select pack carriers. */
long resident_session_bind(struct resident_session *,const char *,const char *);
long resident_session_pack(struct resident_session *,const char *,const char *,const char *);
long resident_session_dns(struct resident_session *,const char *,const char *);
void resident_session_end(struct resident_session *);
long resident_session_call(struct resident_session *,long,const long[6]);
int resident_session_exec(struct resident_session *,const char *,char *const[],char *const[],uintptr_t[2]);
int resident_session_image(struct resident_session *,const uintptr_t[6]);
#endif
