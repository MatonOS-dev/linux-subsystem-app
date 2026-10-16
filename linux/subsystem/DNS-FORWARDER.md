> v3.5: linuxd is retired. Any linuxd/shared-store paths below describe the
> historical design. See [current ownership and device checks](../../docs/RETIRE-LINUXD.md).

# Per-stub DNS forwarder

Each generated app stub runs a native DNS forwarder under its own Android UID.
The loopback address is `127.(10 + ((uid >> 16) & 0x3f)).((uid >> 8) & 0xff).(uid & 0xff)`.
This maps into `127.10.0.0` through `127.73.255.255` and never overlaps
`127.0.0.0/16`. The mapping preserves UID bits 0–21; tests include distinct
multi-user UIDs. The resolver view advertises `address:53`; the stub binds its own UDP and TCP
listeners at `address:<port>` (kernel-assigned, shared by UDP and TCP) before opening the host session. Binding either
socket failing rejects session setup, and partial setup closes both sockets.
No privileged socket, Binder FD relay or linuxd low-port capability is involved.

## Stub ownership and loader translation

The port is unprivileged and kernel-assigned: UDP binds port 0, TCP binds the
same port (retrying with a new UDP port on EADDRINUSE). A fixed port such as
5353 is unsafe because any app's wildcard bind of it would block the stub. The
stub passes the port to the loader (`--dns-forwarder address:port`) at launch,
so there is no discovery step.
The kernel clamps the Java listener's requested maximum backlog to somaxconn.
Sockets close when the stub process exits. The retained native forwarder
implementation still binds a fixed port (not used); production stubs use Java.

V3Launcher gets the logical address directly from the in-process DnsForwarder.
The existing MATON_FLATPAK_DNS monitor writer remains unchanged, as does the
static-apex resolver writer. `/etc/resolv.conf` still says `nameserver address`.

V3Launcher checks that address against its own UID, then passes
`--dns-forwarder address:5353` to the resident loader. Only AF_INET destinations
at that exact address and port 53 are rewritten, using private sockaddr copies:
`connect`, explicit-destination `sendto`, `sendmsg` and every `sendmmsg` entry.
Successful or in-progress translated connects are tracked so `getpeername`
returns port 53 (including truncated buffers and duplicated descriptors).
Direct connects to 5353 and unrelated addresses/ports retain native behavior.
Configuration and descriptor tracking survive fork and resident exec.

The stacked filter traps connect and getpeername; sendto is trapped only when
its address pointer is non-null. BPF cannot read msg_name, so sendmsg/sendmmsg
are always trapped and the handler checks their nested destinations. Other
socket behavior stays native. glibc's connected resolver and musl's unconnected
sendto style are supported. Explicit destinations in these four APIs have no
untranslated case; other mechanisms such as io_uring remain unsupported.
recvfrom/recvmsg source addresses expose the actual 5353 sender; getpeername
is the supported logical-peer view. Descriptor transfers through SCM_RIGHTS,
shared socket reconnects across processes, close_range without subsequent
intercepted creation, and concurrent descriptor-table mutation retain the
proxy's existing compatibility limitations. This is not a containment layer.

## Query attribution and forwarding

For TCP and connected UDP, `NETLINK_SOCK_DIAG` checks the exact source and
destination tuple. For UDP `sendto()` sockets, the worker issues an IPv4 UDP
inet_diag DUMP filtered by the source port from `recvfrom()`. It accepts a
candidate only when its local address is the exact source address or wildcard
`0.0.0.0`. Connected entries must also match the destination tuple. If matching
`SO_REUSEPORT` candidates carry different UIDs, the packet is dropped. Missing,
malformed or incomplete diagnostics fail closed. The host C test constructs a
mock netlink DUMP reply, runs it through the production parser and attribution
selector, and checks truncation and ambiguity handling.

The worker rate-limits queries with a token bucket (100 burst, 50 queries per
second), uses bounded DNS-over-TCP framing, and passes raw packets to
`android_res_nsend(NETWORK_UNSPECIFIED, ...)` followed by
`android_res_nresult()`. This retains Android's default-network, Private DNS,
VPN, per-UID resolver policy and accounting while returning the resolver's
answer bytes.

## SELinux and runtime requirements

linuxd no longer has NET_BIND_SERVICE or the DNS-port name_bind rule. The
bridge/compositor/stub fd and socket grants for linuxd DNS sockets are removed.
The app binds ordinary unprivileged IP sockets and needs NETLINK_SOCK_DIAG
visibility for attribution. Validate these on a fresh enforcing image before
adding any policy; no stock AOSP policy modification is introduced here.

## Host unit test

```sh
cc -std=c11 -Wall -Wextra -Werror \
  -o /tmp/dns-forwarder-core-test \
  linux/subsystem/native/tests/dns_forwarder_core_test.c \
  linux/subsystem/native/dns_forwarder_core.c
/tmp/dns-forwarder-core-test
```

The test covers exact tuple UID checks, mocked UDP inet_diag DUMP attribution
(including wildcard/exact local binds and conflicting reuseport owners),
token-bucket behavior, TCP framing and UID-derived addresses for multiple
Android users.

## Proxy host tests

```sh
bash linux/compat-proxy/tests/run.sh
python3 linux/compat-proxy/tests/dns-host.py
bash linux/compat-proxy/build-android.sh
python3 linux/compat-proxy/tests/lifetime.py /mnt/data/aosp/out/matonos/compat-proxy/libmatonos-v3-loader.so
```

The suite includes a production translation test with a mocked syscall backend
(glibc-style connect and musl-style sendto, message arrays, peer reporting,
unrelated endpoints and invalid configuration). dns-host.py runs actual glibc
res_nquery UDP/TCP lookups and musl-style sendto against local DNS servers,
plus message APIs, dup/peer reporting and native endpoint checks. It needs
AF_INET socket permission; the round-8 sandbox denies socket() with EPERM, so
these live host lookups were not run successfully there. This is a musl syscall
pattern fixture, not a compiled musl resolver.

## Fresh-image validation plan (coordinator)

1. Launch a signed stub; verify that its UID owns UDP and TCP listeners on
   its full-UID-derived address at 5353, and linuxd owns no port-53 listener.
2. Check resolv.conf contains the address alone. Resolve A/AAAA with connected
   UDP/TCP and unconnected sendto, including glibc and an actual musl runtime.
3. Test sendmsg/sendmmsg explicit addresses, logical getpeername :53, unrelated
   endpoints and TCP fallback. Another UID must receive no forwarded answer;
   conflicting SO_REUSEPORT owners must be rejected.
4. Compare Private DNS, VPN, default-network switching and per-UID network rules
   with Android's resolver behavior. Collect sock_diag/socket AVCs in enforcing.
5. Occupy either 5353 listener before launch and confirm session failure and
   closure of any partially bound socket. Close/reopen, force-stop, activity
   recreation and multiple windows must not leak sockets or process trees.

No adb, VM, image or policy build is performed by this task. See the round-8
result report for compile/test evidence and remaining integration checks.
