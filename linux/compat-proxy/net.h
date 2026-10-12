#ifndef V3_NET_H
#define V3_NET_H
long net_call(long nr, long *args, int newnet, int *handled);
void net_close(int fd);
void net_dup(int from, int to);
#endif
