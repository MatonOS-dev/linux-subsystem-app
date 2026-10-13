#ifndef LINKS_H
#define LINKS_H
long links_open(const char *,long,long);
long links_publish(const char *,const char *,long);
void links_close(int);
long links_dup(int,int);
void links_fork(int);
void links_exit(void);
char *links_fd_name(int);
struct stat; struct statx;
void links_fd_stat(int,struct stat *);
void links_recover_path(const char *);
int links_hidden(const char *);
int links_target(const char *,char *);
void links_stat(const char *,struct stat *);
void links_statx(const char *,struct statx *);
long links_path_call(long,long *,const char *);
long links_name_call(long,long *,const char *,const char *);
long links_dents(long,void *,long);
#endif
