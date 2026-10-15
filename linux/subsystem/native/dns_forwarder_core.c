#include "dns_forwarder_core.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <arpa/inet.h>

int dns_forwarder_uid_allowed(dns_uid_lookup_fn lookup, void *context,
                              const void *peer, uint32_t expected_uid) {
    uint32_t actual_uid = UINT32_MAX;
    return lookup != NULL && lookup(context, peer, &actual_uid) == 0 &&
           actual_uid == expected_uid;
}

void dns_token_bucket_init(struct dns_token_bucket *bucket, double capacity,
                           double tokens_per_second, uint64_t now_ns) {
    if (bucket == NULL) return;
    bucket->capacity = capacity > 0 ? capacity : 1;
    bucket->tokens_per_second = tokens_per_second > 0 ? tokens_per_second : 1;
    bucket->tokens = bucket->capacity;
    bucket->last_ns = now_ns;
}

int dns_token_bucket_take(struct dns_token_bucket *bucket, uint64_t now_ns) {
    if (bucket == NULL) return 0;
    if (now_ns > bucket->last_ns) {
        double elapsed = (double)(now_ns - bucket->last_ns) / 1000000000.0;
        bucket->tokens += elapsed * bucket->tokens_per_second;
        if (bucket->tokens > bucket->capacity) bucket->tokens = bucket->capacity;
        bucket->last_ns = now_ns;
    }
    if (bucket->tokens < 1.0) return 0;
    bucket->tokens -= 1.0;
    return 1;
}

int dns_tcp_frame_length(const uint8_t prefix[2], size_t *frame_length) {
    if (prefix == NULL || frame_length == NULL) return -1;
    size_t length = ((size_t)prefix[0] << 8) | prefix[1];
    if (length == 0 || length > DNS_FORWARDER_MAX_PACKET) return -1;
    *frame_length = length;
    return 0;
}

int dns_tcp_frame_write(uint8_t *output, size_t capacity, const uint8_t *message,
                        size_t message_length, size_t *written) {
    if (output == NULL || message == NULL || written == NULL ||
        message_length == 0 || message_length > DNS_FORWARDER_MAX_PACKET ||
        capacity < message_length + DNS_FORWARDER_TCP_PREFIX) return -1;
    output[0] = (uint8_t)(message_length >> 8);
    output[1] = (uint8_t)message_length;
    memcpy(output + DNS_FORWARDER_TCP_PREFIX, message, message_length);
    *written = message_length + DNS_FORWARDER_TCP_PREFIX;
    return 0;
}

void dns_forwarder_address(uint32_t uid, char output[16]) {
    if (output == NULL) return;
    snprintf(output, 16, "127.%u.%u.%u", 10 + ((uid >> 16) & 0x3f),
             (uid >> 8) & 0xff, uid & 0xff);
}

int dns_forwarder_udp_uid(const struct dns_udp_diag_socket *sockets, size_t count,
                          struct in_addr source_address, uint16_t source_port,
                          struct in_addr destination_address, uint16_t destination_port,
                          uint32_t *uid) {
    if (sockets == NULL || uid == NULL || source_port == 0) return -1;
    int found = 0;
    uint32_t found_uid = 0;
    /* Prefer the exact connected tuple when inet_diag reports a remote peer. */
    for (size_t i = 0; i < count; ++i) {
        if (sockets[i].local_port != source_port || sockets[i].remote_port == 0 ||
            sockets[i].remote_port != destination_port ||
            sockets[i].remote_address.s_addr != destination_address.s_addr) continue;
        uint32_t address = ntohl(sockets[i].local_address.s_addr);
        uint32_t source = ntohl(source_address.s_addr);
        if (address != source) continue;
        if (found && found_uid != sockets[i].uid) return -1;
        found_uid = sockets[i].uid;
        found = 1;
    }
    if (found) { *uid = found_uid; return 0; }
    for (size_t i = 0; i < count; ++i) {
        if (sockets[i].local_port != source_port) continue;
        if (sockets[i].remote_port != 0) continue;
        uint32_t address = ntohl(sockets[i].local_address.s_addr);
        uint32_t source = ntohl(source_address.s_addr);
        int wildcard = address == INADDR_ANY;
        int loopback = (address >> 24) == 127;
        if (!wildcard && (!loopback || address != source)) continue;
        if (found && found_uid != sockets[i].uid) return -1;
        found_uid = sockets[i].uid;
        found = 1;
    }
    if (!found) return -1;
    *uid = found_uid;
    return 0;
}

int dns_forwarder_parse_udp_dump_reply(const void *reply, size_t reply_length,
                                       uint32_t sequence, uint16_t source_port,
                                       struct dns_udp_diag_socket *sockets,
                                       size_t capacity, size_t *count, int *done) {
    if (reply == NULL || reply_length > INT_MAX || count == NULL || done == NULL ||
        (capacity != 0 && sockets == NULL)) return -1;
    const uint8_t *bytes = reply;
    int remaining = (int)reply_length;
    while (remaining > 0) {
        if (remaining < (int)sizeof(struct nlmsghdr)) return -1;
        const struct nlmsghdr *header = (const struct nlmsghdr *)bytes;
        if (header->nlmsg_len < sizeof(*header) || header->nlmsg_len > (uint32_t)remaining)
            return -1;
        size_t aligned = NLMSG_ALIGN(header->nlmsg_len);
        if (aligned > (size_t)remaining) return -1;
        if (header->nlmsg_seq == sequence) {
            if (header->nlmsg_type == NLMSG_DONE) {
                *done = 1;
                return 0;
            }
            if (header->nlmsg_type == NLMSG_ERROR) return -1;
            if (header->nlmsg_type == SOCK_DIAG_BY_FAMILY) {
                if (header->nlmsg_len < NLMSG_LENGTH(sizeof(struct inet_diag_msg))) return -1;
                const struct inet_diag_msg *entry = NLMSG_DATA(header);
                if (entry->idiag_family == AF_INET &&
                    entry->id.idiag_sport == htons(source_port)) {
                    if (*count >= capacity) return -1;
                    sockets[*count] = (struct dns_udp_diag_socket){
                        .uid = entry->idiag_uid,
                        .local_address = {.s_addr = entry->id.idiag_src[0]},
                        .local_port = ntohs(entry->id.idiag_sport),
                        .remote_address = {.s_addr = entry->id.idiag_dst[0]},
                        .remote_port = ntohs(entry->id.idiag_dport),
                    };
                    ++*count;
                }
            }
        }
        bytes += aligned;
        remaining -= (int)aligned;
    }
    return 0;
}
