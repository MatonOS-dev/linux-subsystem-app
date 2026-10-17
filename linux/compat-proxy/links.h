#include "arch/arch.h"
#ifndef LINKS_H
#define LINKS_H
long links_open(const char *,long,long);
long links_publish(const char *,const char *,long);
long links_close_fd(int);
long links_dup_fd(long,proxy_arg_t *);
long links_fork(int);
void links_exit(void);
char *links_fd_name(int);
struct proxy_stat; struct statx;
void links_payload_fd_stat(int,void *);
int links_fd_stored(int);
void links_fd_stat(int,struct proxy_stat *);
void links_recover_path(const char *);
int links_hidden(const char *);
int links_target(const char *,char *);
void links_stat(const char *,struct proxy_stat *);
void links_statx(const char *,struct statx *);
long links_path_call(long,proxy_arg_t *,const char *);
long links_name_call(long,proxy_arg_t *,const char *,const char *);
long links_dents(long,void *,long);
#endif

int links_private_fd(int);
