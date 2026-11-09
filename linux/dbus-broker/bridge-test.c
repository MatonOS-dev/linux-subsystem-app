/* Basic bridge ownership, denylist and EOF lifecycle checks. */
#include "broker.c"
#include <assert.h>

int main(void) {
    char directory[] = "/tmp/maton-bridge-XXXXXX"; assert(mkdtemp(directory));
    char policy[128], bus[128]; snprintf(policy,sizeof(policy),"%s/policy",directory); snprintf(bus,sizeof(bus),"%s/bus",directory);
    assert(g_file_set_contents(policy,"own *\ntalk *\n",-1,NULL));
    Broker *b=broker_new(bus,policy,NULL); assert(b);
    int pair[2]; assert(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)==0);
    const char *names[]={"org.matonos.test.Echo","org.freedesktop.secrets","org.kde.kwalletd5","org.freedesktop.portal.Test",NULL};
    GError *error=NULL; assert(broker_add_bridge(b,pair[0],names,&error)); assert(error==NULL);
    assert(g_strcmp0(owner_of(b,"org.matonos.test.Echo"),":1.1")==0);
    assert(owner_of(b,"org.freedesktop.secrets")==NULL);
    assert(owner_of(b,"org.kde.kwalletd5")==NULL);
    assert(owner_of(b,"org.freedesktop.portal.Test")==NULL);
    close(pair[1]);
    while(g_main_context_iteration(NULL,FALSE));
    assert(owner_of(b,"org.matonos.test.Echo")==NULL);
    broker_free(b); unlink(policy); rmdir(directory);
    puts("PASS: bridge attaches, denylist rejects reserved names, EOF releases owned names"); return 0;
}
