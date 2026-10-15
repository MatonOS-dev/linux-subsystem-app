#include "../dns_forwarder_core.h"

#include <assert.h>
#include <arpa/inet.h>
#include <linux/sock_diag.h>
#include <stdio.h>
#include <string.h>

struct mock_diag { int result; uint32_t uid; };
static int mock_uid(void *context, const void *peer, uint32_t *uid) {
    struct mock_diag *diag = context;
    if (peer != diag || diag->result != 0) return -1;
    *uid = diag->uid;
    return 0;
}

static size_t append_diag_entry(uint8_t *reply, size_t offset, uint32_t sequence,
                                uint16_t message_type,
                                const struct inet_diag_msg *entry) {
    struct nlmsghdr *header = (struct nlmsghdr *)(reply + offset);
    header->nlmsg_len = entry == NULL ? sizeof(*header) : NLMSG_LENGTH(sizeof(*entry));
    header->nlmsg_type = message_type;
    header->nlmsg_seq = sequence;
    if (entry != NULL) memcpy(NLMSG_DATA(header), entry, sizeof(*entry));
    return offset + NLMSG_ALIGN(header->nlmsg_len);
}

static struct inet_diag_msg diag_entry(uint32_t uid, struct in_addr local,
                                       uint16_t local_port, struct in_addr remote,
                                       uint16_t remote_port) {
    struct inet_diag_msg entry = {.idiag_family = AF_INET, .idiag_uid = uid};
    entry.id.idiag_src[0] = local.s_addr;
    entry.id.idiag_sport = htons(local_port);
    entry.id.idiag_dst[0] = remote.s_addr;
    entry.id.idiag_dport = htons(remote_port);
    return entry;
}

int main(void) {
    struct mock_diag diag = {0, 10123};
    assert(dns_forwarder_uid_allowed(mock_uid, &diag, &diag, 10123));
    assert(!dns_forwarder_uid_allowed(mock_uid, &diag, &diag, 10124));
    assert(!dns_forwarder_uid_allowed(mock_uid, &diag, &diag, 0));
    diag.result = -1;
    assert(!dns_forwarder_uid_allowed(mock_uid, &diag, &diag, 10123));
    assert(!dns_forwarder_uid_allowed(NULL, &diag, &diag, 10123));

    struct dns_token_bucket bucket;
    dns_token_bucket_init(&bucket, 2, 1, 1000000000ULL);
    assert(dns_token_bucket_take(&bucket, 1000000000ULL));
    assert(dns_token_bucket_take(&bucket, 1000000000ULL));
    assert(!dns_token_bucket_take(&bucket, 1000000000ULL));
    assert(dns_token_bucket_take(&bucket, 2000000000ULL));

    uint8_t prefix[2] = {0x01, 0x00};
    size_t length = 0;
    assert(dns_tcp_frame_length(prefix, &length) == 0 && length == 256);
    prefix[0] = prefix[1] = 0;
    assert(dns_tcp_frame_length(prefix, &length) != 0);
    prefix[0] = 0xff; prefix[1] = 0xff;
    assert(dns_tcp_frame_length(prefix, &length) == 0 && length == 65535);

    const uint8_t message[] = {0xab, 0xcd, 0x00, 0x01};
    uint8_t framed[16] = {0};
    size_t written = 0;
    assert(dns_tcp_frame_write(framed, sizeof(framed), message, sizeof(message), &written) == 0);
    assert(written == 6 && framed[0] == 0 && framed[1] == 4);
    assert(memcmp(framed + 2, message, sizeof(message)) == 0);
    assert(dns_tcp_frame_write(framed, 5, message, sizeof(message), &written) != 0);

    struct in_addr source, destination, other;
    assert(inet_pton(AF_INET, "127.0.0.1", &source) == 1);
    assert(inet_pton(AF_INET, "127.10.39.16", &destination) == 1);
    assert(inet_pton(AF_INET, "127.0.0.2", &other) == 1);
    /* Mock a raw inet_diag DUMP reply with unrelated, wildcard, exact and
     * connected entries, followed by NLMSG_DONE. */
    struct in_addr wildcard = {.s_addr = htonl(INADDR_ANY)};
    struct inet_diag_msg reply_entries[] = {
        diag_entry(22, source, 40124, wildcard, 0),
        diag_entry(10123, wildcard, 40123, wildcard, 0),
        diag_entry(44, other, 40123, wildcard, 0),
        diag_entry(55, source, 40123, destination, 53),
    };
    uint8_t reply[512] = {0};
    size_t reply_length = 0;
    for (size_t i = 0; i < sizeof(reply_entries) / sizeof(reply_entries[0]); ++i)
        reply_length = append_diag_entry(reply, reply_length, 7,
                SOCK_DIAG_BY_FAMILY, &reply_entries[i]);
    reply_length = append_diag_entry(reply, reply_length, 7, NLMSG_DONE, NULL);
    struct dns_udp_diag_socket dump[8] = {0};
    size_t dump_count = 0;
    int dump_done = 0;
    assert(dns_forwarder_parse_udp_dump_reply(reply, reply_length, 7, 40123,
            dump, 8, &dump_count, &dump_done) == 0);
    assert(dump_done && dump_count == 3);
    uint32_t owner = 0;
    assert(dns_forwarder_udp_uid(dump, dump_count, source, 40123, destination, 53, &owner) == 0);
    assert(owner == 55);
    dump[2].remote_port = 54;
    assert(dns_forwarder_udp_uid(dump, dump_count, source, 40123, destination, 53, &owner) == 0);
    assert(owner == 10123);
    dump[1].local_address = source;
    dump[1].uid = 44;
    assert(dns_forwarder_udp_uid(dump, dump_count, source, 40123, destination, 53, &owner) != 0);
    assert(dns_forwarder_parse_udp_dump_reply(reply, reply_length - 1, 7, 40123,
            dump, 8, &dump_count, &dump_done) != 0);

    char address[16];
    dns_forwarder_address(0x123456, address);
    assert(strcmp(address, "127.28.52.86") == 0);
    dns_forwarder_address(110001, address);
    assert(strcmp(address, "127.11.173.177") == 0);
    char other_user[16];
    dns_forwarder_address(210001, other_user);
    assert(strcmp(other_user, "127.13.52.81") == 0);
    assert(strcmp(address, other_user) != 0);
    puts("PASS: tuple and UDP dump attribution, token bucket, DNS framing and UID addresses");
    return 0;
}
