/* Regression: an allowed name, same UID, or recycled PID is insufficient
 * to obtain managed-portal authority. Exercise the actual RequestName gate. */
#include "broker.c"

int main(void) {
    if(getuid()!=1000) {g_print("SKIP: host session clients require system UID 1000\n");return 77;}
    GCredentials* credentials=g_credentials_new();
    Broker b={.portal_pid=getpid(),.portal_pidfd=-1};
    g_assert_false(managed_portal_credentials(&b,credentials,b.portal_generation));
    b.portal_pidfd=(int)syscall(SYS_pidfd_open,getpid(),0);
    if(b.portal_pidfd<0) {g_print("SKIP: pidfd_open unavailable: %s\n",g_strerror(errno));g_object_unref(credentials);return 77;}
    g_assert_true(managed_portal_credentials(&b,credentials,b.portal_generation));
    g_assert_false(managed_portal_credentials(&b,credentials,b.portal_generation-1));
    b.portal_pid=0;g_assert_false(managed_portal_credentials(&b,credentials,b.portal_generation));
    b.portal_pid=getpid()+1;g_assert_false(managed_portal_credentials(&b,credentials,b.portal_generation));
    b.portal_pid=getpid();
    g_assert_true(g_credentials_set_unix_user(credentials,1001,NULL));
    g_assert_false(managed_portal_credentials(&b,credentials,b.portal_generation));
    g_assert_true(g_credentials_set_unix_user(credentials,1000,NULL));
    close(b.portal_pidfd);
    pid_t child=fork();g_assert_cmpint(child,>, -1);
    if(child==0) {for(;;)pause();}
    b.portal_pidfd=(int)syscall(SYS_pidfd_open,child,0);g_assert_cmpint(b.portal_pidfd,>=,0);
    kill(child,SIGTERM);
    struct pollfd dead={.fd=b.portal_pidfd,.events=POLLIN};
    g_assert_cmpint(poll(&dead,1,5000),==,1);
    /* Even a matching new peer PID cannot revive an exited process's pidfd. */
    b.portal_pid=getpid();g_assert_false(managed_portal_credentials(&b,credentials,b.portal_generation));
    while(waitpid(child,NULL,0)<0 && errno==EINTR){}
    close(b.portal_pidfd);g_object_unref(credentials);
    g_print("PASS: managed portal requires exact live PID, system UID and registration generation; dead pidfd cannot be reused\n");
    return 0;
}
