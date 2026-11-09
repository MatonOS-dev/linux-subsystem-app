/* End-to-end broker bridge routing, driver parity and disconnect checks. */
#include "broker.c"
#include <assert.h>

static GDBusMessage *call_message(const char *destination, const char *path,
                                  const char *interface, const char *member,
                                  GVariant *body, guint32 serial) {
    GDBusMessage *m = g_dbus_message_new_method_call(destination, path, interface, member);
    g_dbus_message_set_serial(m, serial);
    if (body) g_dbus_message_set_body(m, body);
    return m;
}
static GDBusMessage *signal_message(const char *path, const char *interface,
                                    const char *member, GVariant *body) {
    GDBusMessage *m = g_dbus_message_new_signal(path, interface, member);
    g_dbus_message_set_serial(m, 1);
    if (body) g_dbus_message_set_body(m, body);
    return m;
}
static void send_message(int fd, GDBusMessage *m) {
    GError *error = NULL; gsize length = 0;
    guchar *blob = g_dbus_message_to_blob(m, &length, G_DBUS_CAPABILITY_FLAGS_UNIX_FD_PASSING, &error);
    assert(blob && error == NULL);
    assert(send(fd, blob, length, MSG_NOSIGNAL) == (ssize_t)length);
    g_free(blob);
}
static GDBusMessage *receive_message(int fd) {
    guint8 blob[65536]; ssize_t n = recv(fd, blob, sizeof(blob), MSG_DONTWAIT | MSG_TRUNC);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return NULL;
    assert(n > 0 && (gsize)n <= sizeof(blob));
    GError *error = NULL;
    GDBusMessage *m = g_dbus_message_new_from_blob(blob, (gsize)n,
        G_DBUS_CAPABILITY_FLAGS_UNIX_FD_PASSING, &error);
    assert(m && error == NULL);
    assert(g_dbus_message_get_serial(m) != 0); /* D-Bus forbids serial 0 */
    return m;
}

static void send_fd_message(int fd, GDBusMessage *m, int passfd, gboolean with_rights) {
    GError *error = NULL;
    GUnixFDList *fds = g_unix_fd_list_new();
    assert(g_unix_fd_list_append(fds, passfd, &error) == 0 && error == NULL);
    g_dbus_message_set_unix_fd_list(m, fds);
    g_object_unref(fds);
    gsize length = 0; guchar *blob = g_dbus_message_to_blob(m, &length, G_DBUS_CAPABILITY_FLAGS_UNIX_FD_PASSING, &error);
    assert(blob && error == NULL);
    if (!with_rights) { assert(send(fd, blob, length, MSG_NOSIGNAL) == (ssize_t)length); g_free(blob); return; }
    struct iovec iov = { .iov_base = blob, .iov_len = length };
    char control[CMSG_SPACE(sizeof(int))]; memset(control, 0, sizeof(control));
    struct msghdr msg = {0}; msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = control; msg.msg_controllen = sizeof(control);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(cmsg), &passfd, sizeof(passfd));
    assert(sendmsg(fd, &msg, MSG_NOSIGNAL) == (ssize_t)length);
    g_free(blob);
}
static GDBusMessage *receive_fd_message(int fd) {
    guint8 blob[65536]; char control[CMSG_SPACE(sizeof(int) * 8)];
    struct iovec iov = { .iov_base = blob, .iov_len = sizeof(blob) };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control) };
    ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return NULL;
    assert(n > 0 && !(msg.msg_flags & MSG_CTRUNC));
    GError *error = NULL;
    GDBusMessage *m = g_dbus_message_new_from_blob(blob, (gsize)n, G_DBUS_CAPABILITY_FLAGS_UNIX_FD_PASSING, &error);
    assert(m && error == NULL);
    assert(g_dbus_message_get_serial(m) != 0); /* D-Bus forbids serial 0 */
    GUnixFDList *fds = g_unix_fd_list_new();
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        assert(cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS);
        int count = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        int *received = (int *)CMSG_DATA(cmsg);
        for (int i = 0; i < count; i++) { assert(g_unix_fd_list_append(fds, received[i], &error) >= 0 && error == NULL); close(received[i]); }
    }
    if (g_unix_fd_list_get_length(fds)) g_dbus_message_set_unix_fd_list(m, fds);
    g_object_unref(fds);
    return m;
}

static void drain_context(void) { while (g_main_context_iteration(NULL, FALSE)); }
static void attach(Broker *b, int *pair, const char *const *names) {
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    GError *error = NULL;
    assert(broker_add_bridge(b, pair[0], names, &error));
    assert(error == NULL);
}
static void add_match(int fd, guint32 serial, const char *rule) {
    GDBusMessage *m = call_message("org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "AddMatch", g_variant_new("(s)", rule), serial);
    send_message(fd, m); g_object_unref(m); drain_context();
    GDBusMessage *reply = receive_message(fd);
    assert(reply && g_dbus_message_get_message_type(reply) == G_DBUS_MESSAGE_TYPE_METHOD_RETURN);
    g_object_unref(reply);
}
static void expect_name_change(int fd, const char *name, const char *old_owner,
                               const char *new_owner) {
    GDBusMessage *m = receive_message(fd);
    assert(m && g_dbus_message_get_message_type(m) == G_DBUS_MESSAGE_TYPE_SIGNAL);
    assert(g_strcmp0(g_dbus_message_get_member(m), "NameOwnerChanged") == 0);
    const char *got_name, *got_old, *got_new;
    g_variant_get(g_dbus_message_get_body(m), "(&s&s&s)", &got_name, &got_old, &got_new);
    assert(g_strcmp0(got_name, name) == 0);
    assert(g_strcmp0(got_old, old_owner) == 0);
    assert(g_strcmp0(got_new, new_owner) == 0);
    g_object_unref(m);
}

int main(void) {
    char directory[] = "/tmp/maton-bridge-XXXXXX"; assert(mkdtemp(directory));
    char policy[128], bus[128]; snprintf(policy,sizeof(policy),"%s/policy",directory); snprintf(bus,sizeof(bus),"%s/bus",directory);
    assert(g_file_set_contents(policy,"own *\ntalk *\n",-1,NULL));
    Broker *b=broker_new(bus,policy,NULL); assert(b);

    /* An observer sees both name acquisition on attach and release on EOF. */
    int observer[2]; const char *no_names[]={NULL}; attach(b,observer,no_names);
    add_match(observer[1],1,"type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'");
    const char *echo_names[]={"org.matonos.test.Echo","org.freedesktop.secrets",
        "org.kde.kwalletd5","org.freedesktop.portal.Test","org.freedesktop.DBus.Test",NULL};
    int provider[2]; attach(b,provider,echo_names);
    expect_name_change(observer[1],"org.matonos.test.Echo","",":1.2");
    /* The owner receives its standard NameAcquired on attachment. */
    GDBusMessage *acquired=receive_message(provider[1]);assert(acquired && g_strcmp0(g_dbus_message_get_member(acquired),"NameAcquired")==0);g_object_unref(acquired);
    assert(g_strcmp0(owner_of(b,"org.matonos.test.Echo"),":1.2")==0);
    assert(owner_of(b,"org.freedesktop.secrets")==NULL);
    assert(owner_of(b,"org.kde.kwalletd5")==NULL);
    assert(owner_of(b,"org.freedesktop.portal.Test")==NULL);
    assert(owner_of(b,"org.freedesktop.DBus.Test")==NULL);

    /* A conflicting bridge name queues and takes over when its owner reaches EOF. */
    const char *queue_owner_names[]={"org.matonos.test.Queue",NULL};
    int queue_owner[2]; attach(b,queue_owner,queue_owner_names);
    expect_name_change(observer[1],"org.matonos.test.Queue","",":1.3");
    GDBusMessage *queue_acquired=receive_message(queue_owner[1]);assert(queue_acquired && g_strcmp0(g_dbus_message_get_member(queue_acquired),"NameAcquired")==0);g_object_unref(queue_acquired);
    const char *queue_waiter_names[]={"org.matonos.test.Queue",NULL};
    int queue_waiter[2]; attach(b,queue_waiter,queue_waiter_names);
    assert(g_strcmp0(owner_of(b,"org.matonos.test.Queue"),":1.3")==0);
    close(queue_owner[1]);drain_context();
    expect_name_change(observer[1],"org.matonos.test.Queue",":1.3",":1.4");
    GDBusMessage *taken=receive_message(queue_waiter[1]);assert(taken && g_strcmp0(g_dbus_message_get_member(taken),"NameAcquired")==0);g_object_unref(taken);
    assert(g_strcmp0(owner_of(b,"org.matonos.test.Queue"),":1.4")==0);

    /* Bridge client installs a match; provider's broadcast signal is delivered. */
    int client[2]; const char *client_names[]={"org.matonos.test.Client",NULL}; attach(b,client,client_names);
    expect_name_change(observer[1],"org.matonos.test.Client","",":1.5");
    GDBusMessage *client_acquired=receive_message(client[1]);assert(client_acquired && g_strcmp0(g_dbus_message_get_member(client_acquired),"NameAcquired")==0);g_object_unref(client_acquired);
    add_match(client[1],1,"type='signal',interface='org.matonos.test.Echo',member='Pinged'");
    GDBusMessage *signal=signal_message("/org/matonos/test/Echo","org.matonos.test.Echo","Pinged",g_variant_new("(s)","4242"));
    send_message(provider[1],signal);g_object_unref(signal);drain_context();
    GDBusMessage *received=receive_message(client[1]);
    assert(received && g_dbus_message_get_message_type(received)==G_DBUS_MESSAGE_TYPE_SIGNAL);
    assert(g_strcmp0(g_dbus_message_get_sender(received),":1.2")==0);
    assert(g_strcmp0(g_dbus_message_get_member(received),"Pinged")==0);g_object_unref(received);

    /* Echo(s)->s makes a complete provider-to-client method round trip. */
    GDBusMessage *echo=call_message("org.matonos.test.Echo","/org/matonos/test/Echo","org.matonos.test.Echo","Echo",g_variant_new("(s)","hello"),22);
    send_message(client[1],echo);g_object_unref(echo);drain_context();
    GDBusMessage *forward=receive_message(provider[1]);
    assert(forward && g_dbus_message_get_message_type(forward)==G_DBUS_MESSAGE_TYPE_METHOD_CALL);
    assert(g_strcmp0(g_dbus_message_get_sender(forward),":1.5")==0);
    GDBusMessage *echo_reply=g_dbus_message_new_method_reply(forward);g_dbus_message_set_body(echo_reply,g_variant_new("(s)","hello"));
    send_message(provider[1],echo_reply);g_object_unref(echo_reply);g_object_unref(forward);drain_context();
    GDBusMessage *roundtrip=receive_message(client[1]);
    assert(roundtrip && g_dbus_message_get_message_type(roundtrip)==G_DBUS_MESSAGE_TYPE_METHOD_RETURN);
    assert(g_dbus_message_get_reply_serial(roundtrip)==22);const char *echo_text;g_variant_get(g_dbus_message_get_body(roundtrip),"(&s)",&echo_text);assert(g_strcmp0(echo_text,"hello")==0);g_object_unref(roundtrip);

    /* A pipe fd travels client -> bridge provider and EchoFd returns its first line. */
    int pipe_fds[2];assert(pipe(pipe_fds)==0);assert(write(pipe_fds[1],"bridge fd line\nignored\n",23)==23);close(pipe_fds[1]);
    GDBusMessage *fd_call=call_message("org.matonos.test.Echo","/org/matonos/test/Echo","org.matonos.test.Echo","EchoFd",g_variant_new("(h)",0),90);
    send_fd_message(client[1],fd_call,pipe_fds[0],TRUE);g_object_unref(fd_call);drain_context();
    GDBusMessage *fd_forward=receive_fd_message(provider[1]);assert(fd_forward && g_dbus_message_get_num_unix_fds(fd_forward)==1);
    GUnixFDList *forward_fds=g_dbus_message_get_unix_fd_list(fd_forward);int forwarded_fd=g_unix_fd_list_get(forward_fds,0,NULL);assert(forwarded_fd>=0);
    char line[64]={0};assert(read(forwarded_fd,line,sizeof(line)-1)>0);assert(g_str_has_prefix(line,"bridge fd line\n"));close(forwarded_fd);
    GDBusMessage *fd_reply=g_dbus_message_new_method_reply(fd_forward);g_dbus_message_set_body(fd_reply,g_variant_new("(s)","bridge fd line"));send_message(provider[1],fd_reply);g_object_unref(fd_reply);g_object_unref(fd_forward);close(pipe_fds[0]);drain_context();
    GDBusMessage *fd_result=receive_message(client[1]);assert(fd_result && g_dbus_message_get_message_type(fd_result)==G_DBUS_MESSAGE_TYPE_METHOD_RETURN);
    const char *fd_text;g_variant_get(g_dbus_message_get_body(fd_result),"(&s)",&fd_text);assert(g_strcmp0(fd_text,"bridge fd line")==0);g_object_unref(fd_result);

    /* A bridge can invoke a method on another bridge's unique client. */
    GDBusMessage *reverse=call_message(":1.5","/","org.freedesktop.DBus.Peer","Ping",NULL,44);
    send_message(provider[1],reverse);g_object_unref(reverse);drain_context();
    GDBusMessage *incoming=receive_message(client[1]);assert(incoming && g_dbus_message_get_message_type(incoming)==G_DBUS_MESSAGE_TYPE_METHOD_CALL);
    GDBusMessage *pong=g_dbus_message_new_method_reply(incoming);send_message(client[1],pong);g_object_unref(pong);g_object_unref(incoming);drain_context();
    GDBusMessage *back=receive_message(provider[1]);assert(back && g_dbus_message_get_message_type(back)==G_DBUS_MESSAGE_TYPE_METHOD_RETURN);g_object_unref(back);

    /* Shared bus-driver implementation: bridge AddMatch and GetNameOwner. */
    GDBusMessage *owner_call=call_message("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","GetNameOwner",g_variant_new("(s)","org.matonos.test.Echo"),55);
    send_message(client[1],owner_call);g_object_unref(owner_call);drain_context();
    GDBusMessage *owner_reply=receive_message(client[1]);assert(owner_reply && g_dbus_message_get_message_type(owner_reply)==G_DBUS_MESSAGE_TYPE_METHOD_RETURN);
    const char *owner;g_variant_get(g_dbus_message_get_body(owner_reply),"(&s)",&owner);assert(g_strcmp0(owner,":1.2")==0);g_object_unref(owner_reply);

    /* A call pending against the provider receives an error when its socket reaches EOF. */
    GDBusMessage *pending=call_message("org.matonos.test.Echo","/org/matonos/test/Echo","org.matonos.test.Echo","Echo",g_variant_new("(s)","wait"),66);
    send_message(client[1],pending);g_object_unref(pending);drain_context();
    GDBusMessage *held=receive_message(provider[1]);assert(held);g_object_unref(held);
    close(provider[1]);drain_context();
    GDBusMessage *failure=receive_message(client[1]);assert(failure && g_dbus_message_get_message_type(failure)==G_DBUS_MESSAGE_TYPE_ERROR);
    assert(g_strcmp0(g_dbus_message_get_error_name(failure),"org.freedesktop.DBus.Error.ServiceUnknown")==0);g_object_unref(failure);
    expect_name_change(observer[1],"org.matonos.test.Echo",":1.2","");
    assert(owner_of(b,"org.matonos.test.Echo")==NULL);

    /* Bridge bus calls are the real bus driver, including malformed signatures. */
    GDBusMessage *invalid=call_message("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","RequestName",g_variant_new("(s)","bad"),77);
    send_message(client[1],invalid);g_object_unref(invalid);drain_context();
    GDBusMessage *invalid_reply=receive_message(client[1]);assert(invalid_reply && g_dbus_message_get_message_type(invalid_reply)==G_DBUS_MESSAGE_TYPE_ERROR);
    assert(g_strcmp0(g_dbus_message_get_error_name(invalid_reply),"org.freedesktop.DBus.Error.InvalidArgs")==0);g_object_unref(invalid_reply);

    /* Bridge peer credentials are unavailable; never return the broker UID. */
    const char *credential_methods[]={"GetConnectionUnixUser","GetConnectionUnixProcessID","GetConnectionCredentials",NULL};
    for(guint i=0;credential_methods[i];i++){
        GDBusMessage *cred=call_message("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus",credential_methods[i],g_variant_new("(s)",":1.5"),80+i);
        send_message(client[1],cred);g_object_unref(cred);drain_context();
        GDBusMessage *cred_error=receive_message(client[1]);assert(cred_error && g_dbus_message_get_message_type(cred_error)==G_DBUS_MESSAGE_TYPE_ERROR);
        assert(g_strcmp0(g_dbus_message_get_error_name(cred_error),"org.freedesktop.DBus.Error.Failed")==0);
        const char *cred_text;g_variant_get(g_dbus_message_get_body(cred_error),"(&s)",&cred_text);assert(g_strcmp0(cred_text,"bridge peer has no Unix credentials")==0);g_object_unref(cred_error);
    }

    /* UNIX_FDS header/SCM_RIGHTS mismatch drops only that datagram. */
    GDBusMessage *mismatch=call_message("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus.Peer","Ping",NULL,91);
    int mismatch_fd=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(mismatch_fd>=0);
    send_fd_message(client[1],mismatch,mismatch_fd,FALSE);close(mismatch_fd);g_object_unref(mismatch);drain_context();
    assert(receive_message(client[1])==NULL);
    GDBusMessage *alive=call_message("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus.Peer","Ping",NULL,92);
    send_message(client[1],alive);g_object_unref(alive);drain_context();GDBusMessage *alive_reply=receive_message(client[1]);
    assert(alive_reply && g_dbus_message_get_message_type(alive_reply)==G_DBUS_MESSAGE_TYPE_METHOD_RETURN);g_object_unref(alive_reply);

    close(client[1]);close(observer[1]);close(queue_waiter[1]);drain_context();broker_free(b);unlink(policy);rmdir(directory);
    puts("PASS: bridge Echo/reverse calls, broadcast matches, bus-driver parity, attach/EOF name changes and pending errors");
    return 0;
}
