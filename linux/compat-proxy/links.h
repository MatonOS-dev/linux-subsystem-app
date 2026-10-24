#include "arch/arch.h"
#ifndef LINKS_H
#define LINKS_H
long links_publish(const char *,const char *,long);
struct proxy_stat; struct statx;
void links_payload_fd_stat(int,void *);
void links_fd_stat(int,struct proxy_stat *);
void links_recover_path(const char *);
int links_hidden(const char *);
int links_target(const char *,char *);
void links_stat(const char *,struct proxy_stat *);
long links_path_call(long,proxy_arg_t *,const char *);
long links_name_call(long,proxy_arg_t *,const char *,const char *);
long links_dents(long,void *,long);
#endif


int links_marker(const char *);
void links_cache_invalidate(void);
