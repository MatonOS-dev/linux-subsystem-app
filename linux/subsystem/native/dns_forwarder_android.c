#define _GNU_SOURCE
#include "dns_forwarder_core.h"

#include <android/multinetwork.h>
#include <arpa/inet.h>
#include <errno.h>
#include <jni.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DNS_PORT 5353
#define DNS_RATE_CAPACITY 100.0
#define DNS_RATE_PER_SECOND 50.0
#define DNS_RESOLVER_TIMEOUT_MS 10000

struct peer_tuple {
    struct in_addr source;
    struct in_addr destination;
    uint16_t source_port;
    uint16_t destination_port;
    int protocol;
};

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static int worker_started;
static int stopping;
static int udp_fd = -1;
static int tcp_fd = -1;
static int wake_fd = -1;
static uint32_t service_uid;
static int service_port;
static struct in_addr service_address;
static struct dns_token_bucket rate_bucket;
static pthread_mutex_t rate_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/* Connected flows use an exact tuple query. */
static int lookup_peer_uid(void *unused, const void *opaque, uint32_t *uid) {
    (void)unused;
    const struct peer_tuple *peer = opaque;
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0) return -1;
    struct {
        struct nlmsghdr header;
        struct inet_diag_req_v2 request;
    } query = {0};
    query.header.nlmsg_len = sizeof(query);
    query.header.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    query.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    query.header.nlmsg_seq = 1;
    query.request.sdiag_family = AF_INET;
    query.request.sdiag_protocol = (uint8_t)peer->protocol;
    query.request.idiag_states = UINT32_MAX;
    query.request.id.idiag_sport = htons(peer->source_port);
    query.request.id.idiag_dport = htons(peer->destination_port);
    query.request.id.idiag_src[0] = peer->source.s_addr;
    query.request.id.idiag_dst[0] = peer->destination.s_addr;
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    if (sendto(fd, &query, sizeof(query), 0, (struct sockaddr *)&kernel,
               sizeof(kernel)) < 0) { close(fd); return -1; }

    uint8_t buffer[16384];
    int found = 0;
    for (;;) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 1000) <= 0) break;
        ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        int remaining = (int)count;
        int message_size = 0;
        for (struct nlmsghdr *header = (struct nlmsghdr *)buffer;
             remaining >= (int)sizeof(*header) &&
             header->nlmsg_len >= sizeof(*header) &&
             header->nlmsg_len <= (uint32_t)remaining;
             header = (struct nlmsghdr *)((uint8_t *)header + message_size),
             remaining -= message_size) {
            message_size = (int)NLMSG_ALIGN(header->nlmsg_len);
            if (header->nlmsg_type == NLMSG_DONE) goto done;
            if (header->nlmsg_type == NLMSG_ERROR ||
                header->nlmsg_len < NLMSG_LENGTH(sizeof(struct inet_diag_msg))) continue;
            struct inet_diag_msg *entry = NLMSG_DATA(header);
            if (entry->idiag_family != AF_INET) continue;
            if (entry->id.idiag_sport != htons(peer->source_port) ||
                entry->id.idiag_dport != htons(peer->destination_port) ||
                entry->id.idiag_src[0] != peer->source.s_addr ||
                entry->id.idiag_dst[0] != peer->destination.s_addr) continue;
            *uid = entry->idiag_uid;
            found = 1;
            goto done;
        }
    }
done:
    close(fd);
    return found ? 0 : -1;
}

/* Unconnected sendto() sockets have no remote tuple. Dump only sockets with
 * the sender's local port, then authorize an exact loopback bind or wildcard
 * bind. Conflicting SO_REUSEPORT owners are rejected by the core selector. */
static int lookup_udp_uid(const struct sockaddr_in *source, uint32_t *uid) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0) return -1;
    struct {
        struct nlmsghdr header;
        struct inet_diag_req_v2 request;
    } query = {0};
    query.header.nlmsg_len = sizeof(query);
    query.header.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    query.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    query.header.nlmsg_seq = 1;
    query.request.sdiag_family = AF_INET;
    query.request.sdiag_protocol = IPPROTO_UDP;
    query.request.idiag_states = UINT32_MAX;
    query.request.id.idiag_sport = source->sin_port;
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    if (sendto(fd, &query, sizeof(query), 0, (struct sockaddr *)&kernel,
               sizeof(kernel)) < 0) { close(fd); return -1; }

    struct dns_udp_diag_socket entries[1024];
    size_t count = 0;
    uint8_t buffer[65536];
    int complete = 0;
    while (!complete) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 1000) <= 0) break;
        struct iovec iov = {.iov_base = buffer, .iov_len = sizeof(buffer)};
        struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1};
        ssize_t received = recvmsg(fd, &message, MSG_TRUNC);
        if (received <= 0) break;
        if ((size_t)received > sizeof(buffer) || (message.msg_flags & MSG_TRUNC)) {
            complete = -1; break;
        }
        int dump_done = 0;
        if (dns_forwarder_parse_udp_dump_reply(buffer, (size_t)received, 1,
                ntohs(source->sin_port), entries, 1024, &count, &dump_done) != 0) {
            complete = -1;
            break;
        }
        if (dump_done) complete = 1;
    }
    close(fd);
    if (complete != 1) return -1;
    struct in_addr destination = service_address;
    return dns_forwarder_udp_uid(entries, count, source->sin_addr,
                                 ntohs(source->sin_port), destination,
                                 (uint16_t)service_port, uid);
}

static int authorize(const struct sockaddr_in *source, int protocol) {
    struct peer_tuple peer = {.source = source->sin_addr,
        .destination = service_address,
        .source_port = ntohs(source->sin_port),
        .destination_port = (uint16_t)service_port, .protocol = protocol};
    return dns_forwarder_uid_allowed(lookup_peer_uid, NULL, &peer, service_uid);
}

static int allow_query(void) {
    pthread_mutex_lock(&rate_lock);
    int allowed = dns_token_bucket_take(&rate_bucket, monotonic_ns());
    pthread_mutex_unlock(&rate_lock);
    return allowed;
}

static int resolve_packet(const uint8_t *query, size_t query_length,
                          uint8_t *answer, size_t answer_capacity) {
    int query_fd = android_res_nsend(0, query, query_length, 0);
    if (query_fd < 0) return -1;
    struct pollfd pfd = {.fd = query_fd, .events = POLLIN};
    int ready;
    do { ready = poll(&pfd, 1, DNS_RESOLVER_TIMEOUT_MS); } while (ready < 0 && errno == EINTR);
    if (ready <= 0) { android_res_cancel(query_fd); return -1; }
    int rcode = 0;
    return android_res_nresult(query_fd, &rcode, answer, answer_capacity);
}

static void serve_udp(void) {
    uint8_t query[DNS_FORWARDER_MAX_PACKET], answer[DNS_FORWARDER_MAX_PACKET];
    struct sockaddr_in source;
    socklen_t source_length = sizeof(source);
    ssize_t size = recvfrom(udp_fd, query, sizeof(query), 0,
                            (struct sockaddr *)&source, &source_length);
    uint32_t sender_uid = UINT32_MAX;
    if (size <= 0 || source.sin_family != AF_INET ||
        lookup_udp_uid(&source, &sender_uid) != 0 || sender_uid != service_uid ||
        !allow_query()) return;
    int result = resolve_packet(query, (size_t)size, answer, sizeof(answer));
    if (result > 0 && result <= (int)sizeof(answer))
        sendto(udp_fd, answer, (size_t)result, 0, (struct sockaddr *)&source, source_length);
}

static int read_exact(int fd, uint8_t *data, size_t length) {
    size_t used = 0;
    while (used < length) {
        ssize_t n = recv(fd, data + used, length - used, 0);
        if (n <= 0) return -1;
        used += (size_t)n;
    }
    return 0;
}

static void serve_tcp(void) {
    int client = accept4(tcp_fd, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) return;
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in source;
    socklen_t source_length = sizeof(source);
    if (getpeername(client, (struct sockaddr *)&source, &source_length) != 0 ||
        source.sin_family != AF_INET || !authorize(&source, IPPROTO_TCP) || !allow_query()) {
        close(client); return;
    }
    uint8_t prefix[2], query[DNS_FORWARDER_MAX_PACKET], answer[DNS_FORWARDER_MAX_PACKET];
    if (read_exact(client, prefix, sizeof(prefix)) == 0) {
        size_t query_length;
        if (dns_tcp_frame_length(prefix, &query_length) == 0 &&
            read_exact(client, query, query_length) == 0) {
            int answer_length = resolve_packet(query, query_length, answer, sizeof(answer));
            uint8_t framed[DNS_FORWARDER_MAX_PACKET + 2]; size_t framed_length;
            if (answer_length > 0 && dns_tcp_frame_write(framed, sizeof(framed), answer,
                    (size_t)answer_length, &framed_length) == 0) {
                size_t sent = 0;
                while (sent < framed_length) {
                    ssize_t n = send(client, framed + sent, framed_length - sent, MSG_NOSIGNAL);
                    if (n <= 0) break;
                    sent += (size_t)n;
                }
            }
        }
    }
    close(client);
}

static void *forwarder_main(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&state_lock);
        int stop = stopping;
        pthread_mutex_unlock(&state_lock);
        if (stop) break;
        struct pollfd fds[] = {{.fd = udp_fd, .events = POLLIN},
                               {.fd = tcp_fd, .events = POLLIN},
                               {.fd = wake_fd, .events = POLLIN}};
        int ready = poll(fds, 3, -1);
        if (ready > 0 && (fds[2].revents & POLLIN)) break;
        if (ready > 0 && (fds[0].revents & POLLIN)) serve_udp();
        if (ready > 0 && (fds[1].revents & POLLIN)) serve_tcp();
    }
    return NULL;
}

JNIEXPORT jstring JNICALL
Java_org_matonos_linux_stub_DnsForwarder_nativeStart(JNIEnv *env, jclass clazz) {
    (void)clazz;
    pthread_mutex_lock(&state_lock);
    if (worker_started) {
        char address[16], existing[32];
        dns_forwarder_address(service_uid, address);
        snprintf(existing, sizeof(existing), "%s:%d", address, service_port);
        pthread_mutex_unlock(&state_lock);
        return (*env)->NewStringUTF(env, existing);
    }
    service_uid = (uint32_t)getuid();
    char address[16]; dns_forwarder_address(service_uid, address);
    if (inet_pton(AF_INET, address, &service_address) != 1) {
        pthread_mutex_unlock(&state_lock); return NULL;
    }
    service_port = DNS_PORT;
    struct sockaddr_in bind_address = {.sin_family = AF_INET,
        .sin_port = htons(DNS_PORT), .sin_addr = service_address};
    udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    tcp_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (udp_fd < 0 || tcp_fd < 0 ||
            bind(udp_fd, (struct sockaddr *)&bind_address, sizeof(bind_address)) != 0 ||
            bind(tcp_fd, (struct sockaddr *)&bind_address, sizeof(bind_address)) != 0 ||
            listen(tcp_fd, SOMAXCONN) != 0) {
        if (udp_fd >= 0) close(udp_fd);
        if (tcp_fd >= 0) close(tcp_fd);
        udp_fd = tcp_fd = -1; pthread_mutex_unlock(&state_lock); return NULL;
    }
    wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd < 0) {
        close(udp_fd); close(tcp_fd); udp_fd = tcp_fd = -1;
        pthread_mutex_unlock(&state_lock); return NULL;
    }
    stopping = 0;
    dns_token_bucket_init(&rate_bucket, DNS_RATE_CAPACITY, DNS_RATE_PER_SECOND, monotonic_ns());
    if (pthread_create(&worker, NULL, forwarder_main, NULL) != 0) {
        close(udp_fd); close(tcp_fd); close(wake_fd);
        udp_fd = tcp_fd = wake_fd = -1;
        pthread_mutex_unlock(&state_lock); return NULL;
    }
    worker_started = 1;
    char endpoint[32]; snprintf(endpoint, sizeof(endpoint), "%s:%d", address, DNS_PORT);
    pthread_mutex_unlock(&state_lock);
    return (*env)->NewStringUTF(env, endpoint);
}

JNIEXPORT void JNICALL
Java_org_matonos_linux_stub_DnsForwarder_nativeStop(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    pthread_mutex_lock(&state_lock);
    if (!worker_started) { pthread_mutex_unlock(&state_lock); return; }
    stopping = 1;
    uint64_t wake = 1;
    (void)write(wake_fd, &wake, sizeof(wake));
    shutdown(udp_fd, SHUT_RDWR); shutdown(tcp_fd, SHUT_RDWR);
    pthread_mutex_unlock(&state_lock);
    pthread_join(worker, NULL);
    pthread_mutex_lock(&state_lock);
    close(udp_fd); close(tcp_fd); close(wake_fd);
    udp_fd = tcp_fd = wake_fd = -1;
    worker_started = 0;
    pthread_mutex_unlock(&state_lock);
}
