/* The host must not translate systemd-resolved's address into Android netd.
 * No network, root-only listener, or installed resolver service is required. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include "../proxy.c"
int main(void){
    struct sockaddr_in resolved={.sin_family=AF_INET,.sin_port=0x3500,.sin_addr={.s_addr=0x3500007f}};
    assert(!dns_address(&resolved,sizeof(resolved)));
    puts("PASS host resolver 127.0.0.53:53 is native, not redirected to absent Android netd");
}
