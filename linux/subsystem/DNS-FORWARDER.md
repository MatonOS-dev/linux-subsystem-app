# Per-process DNS in the compat proxy

The host app does not run a DNS listener or a separate forwarder service.
Launches and Flatpak installs map a resolver file containing
`nameserver 127.0.0.53` and pass `--dns-forwarder 127.0.0.53:53` to the v3
loader. The compat proxy lazily starts one helper thread per Linux process.
That helper binds an ephemeral UDP port on `127.0.0.1`, submits raw DNS
packets through Android's `android_res_nsend(NETWORK_UNSPECIFIED, ...)`, and
returns answers to the original process socket. This keeps Android's default
network, Private DNS, VPN and per-UID resolver policy in use.

The proxy rewrites UDP destinations to the helper while preserving the app's
socket. It reports the configured logical peer for `getpeername` and receive
source addresses. TCP connections to the configured DNS endpoint fail with
`ECONNREFUSED`; TCP DNS is not supported. Only source ports associated with
AF_INET sockets created by that Linux process are accepted by the helper.

The former Java/native `DnsForwarder` service and its Unix-socket transport
are removed. Host tests for DNS behavior live with the compat proxy and mock
the Android resolver APIs. The no-hardware/ordinary launch path does not
require starting a service or binding a privileged port.

## Validation

The host suite checks endpoint parsing, TCP refusal, ordinary UDP naming, and
endpoint matching. `tests/dns-helper.c` exercises the live helper loop with
mocked resolver calls and real host UDP/epoll/pthreads, including connected
UDP, sendto/sendmsg/sendmmsg, parallel queries, poll/epoll, blocking reads,
`MSG_PEEK`, foreign-source rejection, fork restart, and no unrelated socket
wakeups. Fresh-image testing should verify the Android default network and a
Flatpak install. The old Java listener/UID-attribution checks no longer apply.
