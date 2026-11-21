#define _GNU_SOURCE
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int pollable_link_dump(int fd) {
    if (fd < 0) {
        fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
        if (fd < 0) return -1;
        struct sockaddr_nl local = {.nl_family = AF_NETLINK};
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) goto fail;
    }
    struct sockaddr_nl local = {0};
    socklen_t local_len = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &local_len) != 0 || !local.nl_pid) goto fail;
    int domain = 0, type = 0, protocol = 0;
    socklen_t opt_len = sizeof(int);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &opt_len) != 0 || domain != AF_NETLINK) goto fail;
    opt_len = sizeof(int);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &opt_len) != 0 || type != SOCK_RAW) goto fail;
    opt_len = sizeof(int);
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &protocol, &opt_len) != 0 || protocol != NETLINK_ROUTE) goto fail;
    int copy = dup(fd);
    if (copy < 0) goto fail;
    if (close(fd) != 0) { close(copy); goto fail; }
    fd = copy;
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    struct { struct nlmsghdr h; struct rtgenmsg g; } request = {
        .h = {.nlmsg_len = sizeof(request), .nlmsg_type = RTM_GETLINK,
              .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP, .nlmsg_seq = 71},
        .g = {.rtgen_family = AF_UNSPEC},
    };
    if (sendto(fd, &request, sizeof(request), 0,
               (struct sockaddr *)&kernel, sizeof(kernel)) != sizeof(request)) goto fail;
    int found_lo = 0, done = 0;
    while (!done) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 2000) != 1 || !(pfd.revents & POLLIN)) goto fail;
        unsigned char buf[4096];
        struct sockaddr_nl source = {0};
        struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
        struct msghdr msg = {.msg_name = &source, .msg_namelen = sizeof(source),
                             .msg_iov = &iov, .msg_iovlen = 1};
        ssize_t n = recvmsg(fd, &msg, 0);
        if (n < (ssize_t)sizeof(struct nlmsghdr) || source.nl_family != AF_NETLINK || source.nl_pid) goto fail;
        size_t remain = (size_t)n;
        for (struct nlmsghdr *h = (void *)buf; NLMSG_OK(h, remain); h = NLMSG_NEXT(h, remain)) {
            if (h->nlmsg_seq != request.h.nlmsg_seq || h->nlmsg_pid != local.nl_pid) goto fail;
            if (h->nlmsg_type == NLMSG_DONE) { done = 1; break; }
            if (h->nlmsg_type != RTM_NEWLINK || h->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg))) continue;
            struct ifinfomsg *ifi = NLMSG_DATA(h);
            size_t attrs_len = h->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi));
            for (struct rtattr *a = IFLA_RTA(ifi); RTA_OK(a, attrs_len); a = RTA_NEXT(a, attrs_len))
                if (a->rta_type == IFLA_IFNAME && !strcmp(RTA_DATA(a), "lo") && (ifi->ifi_flags & IFF_UP)) found_lo = 1;
        }
    }
    close(fd);
    return found_lo ? 0 : -1;
fail:
    close(fd);
    return -1;
}

int main(int argc, char **argv) {
    int inherited_fd = -1;
    if (argc == 3 && !strcmp(argv[1], "--continue")) inherited_fd = atoi(argv[2]);
    else if (argc == 1) {
        int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        struct sockaddr_nl local = {.nl_family = AF_NETLINK};
        if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
            perror("route bind before exec");
            return 1;
        }
        char number[32];
        snprintf(number, sizeof(number), "%d", fd);
        pid_t child = fork();
        if (child < 0) { perror("fork route endpoint"); close(fd); return 1; }
        if (child == 0) {
            execl("/proc/self/exe", argv[0], "--continue", number, (char *)NULL);
            perror("exec route endpoint");
            _exit(1);
        }
        close(fd);
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) return 1;
        return WEXITSTATUS(status);
    } else return 2;
    if (pollable_link_dump(inherited_fd) != 0) {
        fprintf(stderr, "FAIL NETLINK_ROUTE dump was not pollable or lacked lo\n");
        return 1;
    }
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all) != 0) {
        perror("getifaddrs");
        return 1;
    }
    int lo_up = 0, lo_inet = 0;
    for (struct ifaddrs *it = all; it; it = it->ifa_next) {
        if (!it->ifa_name || strcmp(it->ifa_name, "lo")) continue;
        if (it->ifa_flags & IFF_UP) lo_up = 1;
        if (it->ifa_addr && it->ifa_addr->sa_family == AF_INET) lo_inet = 1;
    }
    freeifaddrs(all);
    if (!lo_up || !lo_inet) {
        fprintf(stderr, "FAIL getifaddrs lo flags=%d inet=%d\n", lo_up, lo_inet);
        return 1;
    }
    puts("PASS glibc getifaddrs sees lo IFF_UP and AF_INET through compat proxy");
    return 0;
}
