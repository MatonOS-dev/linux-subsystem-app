package org.matonos.linux.stub;

import android.net.DnsResolver;
import android.os.CancellationSignal;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructPollfd;
import android.util.Log;
import java.io.FileDescriptor;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Per-process Unix DNS transport, authenticated by app UID and private
 * filesystem permissions. Resolves through netd as the stub's UID, preserving
 * Private DNS, VPN and firewall policy. No Internet listener or sock_diag. */
final class DnsForwarder {
    private static final String TAG = "MatonDnsForwarder";
    private static final int MAX_PACKET = 65535;
    private static final int RESOLVER_TIMEOUT_MS = 10000;
    private static String socketPath;
    private static AutoCloseable workLease;
    private static org.matonos.linux.ShortSocketPath shortPath;
    private static FileDescriptor udp, tcp;
    private static int uid;
    private static android.net.LocalServerSocket udpServer, tcpServer;
    private static android.net.LocalSocket udpSocket, tcpSocket;
    private static volatile long generation;
    private static final java.util.concurrent.ExecutorService workers =
            new java.util.concurrent.ThreadPoolExecutor(0, Integer.MAX_VALUE, 30, TimeUnit.SECONDS,
                    new java.util.concurrent.SynchronousQueue<>(), runnable -> {
                        Thread t = new Thread(runnable, "maton-dns-client"); t.setDaemon(true); return t;
                    });

    private static final java.util.concurrent.ExecutorService queries =
            new java.util.concurrent.ThreadPoolExecutor(0, Integer.MAX_VALUE, 30, TimeUnit.SECONDS,
                    new java.util.concurrent.SynchronousQueue<>(), runnable -> {
                        Thread t = new Thread(runnable, "maton-dns-query"); t.setDaemon(true); return t;
                    });

    private static final class Client {
        final android.net.LocalSocket socket;
        final FileDescriptor fd;
        boolean closed;
        Client(android.net.LocalSocket socket) { this.socket = socket; fd = socket.getFileDescriptor(); }
        synchronized void answer(byte[] query) throws Exception {
            if (closed) return;
            // A sequenced packet is one entire DNS response, never a fragment.
            if (Os.write(fd, query, 0, query.length) != query.length)
                throw new java.io.IOException("Short DNS packet write");
        }
        synchronized void close() { closed = true; closeQuietly(socket); }
    }

    static synchronized String acquire(java.io.File runtime) throws Exception {
        if (socketPath != null) return socketPath;
        try {
            org.matonos.linux.WorkDirs.checkDnsPath(runtime);
            workLease = org.matonos.linux.WorkDirs.keep(new java.io.File(runtime,"dns"));
            uid = android.os.Process.myUid();
            java.io.File directory = new java.io.File(runtime, "dns");
            if (!directory.isDirectory() && !directory.mkdirs())
                throw new java.io.IOException("Cannot create DNS directory");
            Os.chmod(directory.getCanonicalPath(), 0700);
            shortPath = org.matonos.linux.ShortSocketPath.open(directory);
            String path = new java.io.File(directory, "forwarder").getCanonicalPath();
            udpSocket = listener(shortPath.path("forwarder.udp"), path + ".udp",
                    android.net.LocalSocket.SOCKET_SEQPACKET);
            tcpSocket = listener(shortPath.path("forwarder.tcp"), path + ".tcp",
                    android.net.LocalSocket.SOCKET_STREAM);
            udp = udpSocket.getFileDescriptor(); tcp = tcpSocket.getFileDescriptor();
            udpServer = new android.net.LocalServerSocket(udp);
            tcpServer = new android.net.LocalServerSocket(tcp);
            socketPath = path;
            final long instance = ++generation;
            Thread worker = new Thread(() -> serve(instance), "maton-dns-forwarder");
            worker.setDaemon(true); worker.start();
        } catch (Exception error) {
            Log.e(TAG, "Cannot bind the DNS forwarder", error);
            release();
            throw error;
        }
        return socketPath;
    }

    private static android.net.LocalSocket listener(String bindPath, String filesystemPath, int type) throws Exception {
        java.io.File file = new java.io.File(filesystemPath);
        if (file.exists() && !file.delete()) throw new java.io.IOException("Stale DNS socket");
        android.net.LocalSocket socket = new android.net.LocalSocket(type);
        try {
            socket.bind(new android.net.LocalSocketAddress(bindPath, android.net.LocalSocketAddress.Namespace.FILESYSTEM));
            Os.chmod(filesystemPath, 0600);
            return socket;
        } catch (Exception error) { closeQuietly(socket); throw error; }
    }

    /** Releases the listener for this process, closing both sockets and the
     *  per-client channels. A later {@link #acquire} binds a fresh forwarder,
     *  so a host install never reuses another operation's deleted directory. */
    static synchronized void release() {
        ++generation;
        if (udpServer != null) { try { udpServer.close(); } catch (Exception ignored) { } udpServer = null; }
        if (tcpServer != null) { try { tcpServer.close(); } catch (Exception ignored) { } tcpServer = null; }
        closeQuietly(udpSocket); closeQuietly(tcpSocket);
        udpSocket = tcpSocket = null; udp = tcp = null; socketPath = null;
        if(shortPath!=null)try{shortPath.close();}catch(Exception ignored){}
        shortPath=null;
        if(workLease!=null)try{workLease.close();}catch(Exception ignored){}
        workLease=null;
    }

    static synchronized String endpoint() {
        int uid = android.os.Process.myUid();
        return String.format(java.util.Locale.ROOT, "127.%d.%d.%d:53",
                10 + ((uid >>> 16) & 0x3f), (uid >>> 8) & 0xff, uid & 0xff);
    }

    private static void serve(long instance) {
        StructPollfd[] fds = { pollfd(udp), pollfd(tcp) };
        while (instance == generation) {
            try {
                fds[0].revents = fds[1].revents = 0;
                if (instance != generation) return;
                if (Os.poll(fds, -1) <= 0) continue;
                if (instance != generation) return;
                if (udpServer != null && (fds[0].revents & OsConstants.POLLIN) != 0) accept(udpServer, false);
                if (tcpServer != null && (fds[1].revents & OsConstants.POLLIN) != 0) accept(tcpServer, true);
            } catch (ErrnoException error) {
                if (error.errno != OsConstants.EINTR) { Log.e(TAG, "DNS forwarder stopped", error); return; }
            } catch (Exception error) {
                Log.w(TAG, "DNS query failed", error);
            }
        }
    }

    private static StructPollfd pollfd(FileDescriptor fd) {
        StructPollfd p = new StructPollfd();
        p.fd = fd;
        p.events = (short) OsConstants.POLLIN;
        return p;
    }

    static boolean allowedPeer(int peerUid, int ownUid) { return peerUid == ownUid; }

    private static void accept(android.net.LocalServerSocket listener, boolean stream) throws Exception {
        android.net.LocalSocket client = listener.accept();
        try {
            // SO_PEERCRED is supplied by the kernel, independent of packet data.
            if (!allowedPeer(client.getPeerCredentials().getUid(), uid)) { closeQuietly(client); return; }
            workers.execute(() -> serveClient(client, stream));
        } catch (Exception error) { closeQuietly(client); throw error; }
    }

    private static void serveClient(android.net.LocalSocket socket, boolean stream) {
        Client channel = new Client(socket);
        FileDescriptor client = channel.fd;
        try {
            android.system.StructTimeval timeout = android.system.StructTimeval.fromMillis(15000);
            // UDP sockets remain usable after idle periods, as real UDP sockets do.
            Os.setsockoptTimeval(client, OsConstants.SOL_SOCKET, OsConstants.SO_RCVTIMEO,
                    stream ? timeout : android.system.StructTimeval.fromMillis(0));
            Os.setsockoptTimeval(client, OsConstants.SOL_SOCKET, OsConstants.SO_SNDTIMEO, timeout);
            for (;;) {
                byte[] query;
                if (stream) {
                    byte[] prefix = readExact(client, 2);
                    if (prefix == null) return;
                    int length = ((prefix[0] & 0xff) << 8) | (prefix[1] & 0xff);
                    if (length < 12) return;
                    query = readExact(client, length);
                } else {
                    byte[] buffer = new byte[MAX_PACKET];
                    int n = Os.read(client, buffer, 0, buffer.length);
                    if (n <= 0) return;
                    query = java.util.Arrays.copyOf(buffer, n);
                }
                if (query == null) return;
                if (query.length < 12) { if (stream) return; else continue; }
                if (!stream) {
                    final byte[] packet = query;
                    try {
                        queries.execute(() -> {
                            try { byte[] answer = resolve(packet); if (answer != null) channel.answer(answer); }
                            catch (Exception error) { Log.d(TAG, "DNS query failed", error); }
                        });
                    } catch (java.util.concurrent.RejectedExecutionException busy) { /* Drop; resolver retries. */ }
                    continue;
                }
                byte[] answer = resolve(query);
                // Resolver errors do not close a datagram channel; later queries
                // remain usable. The application's resolver owns its timeout.
                if (answer == null) continue;
                if (stream) {
                    byte[] framed = new byte[answer.length + 2];
                    framed[0] = (byte) (answer.length >>> 8); framed[1] = (byte) answer.length;
                    System.arraycopy(answer, 0, framed, 2, answer.length);
                    for (int sent = 0; sent < framed.length;) {
                        int n = Os.write(client, framed, sent, framed.length - sent);
                        if (n <= 0) return;
                        sent += n;
                    }
                }
            }
        } catch (Exception error) { Log.d(TAG, "DNS client closed", error); }
        finally { channel.close(); }
    }

    private static byte[] readExact(FileDescriptor fd, int length) throws Exception {
        byte[] data = new byte[length];
        for (int used = 0; used < length; ) {
            int n = Os.read(fd, data, used, length - used);
            if (n <= 0) return null;
            used += n;
        }
        return data;
    }

    /** Through netd on the default network, as this stub's UID. */
    private static byte[] resolve(byte[] query) throws InterruptedException {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<byte[]> result = new AtomicReference<>();
        CancellationSignal cancel = new CancellationSignal();
        DnsResolver.getInstance().rawQuery(null, query, DnsResolver.FLAG_EMPTY, Runnable::run, cancel,
                new DnsResolver.Callback<byte[]>() {
                    @Override public void onAnswer(byte[] answer, int rcode) { result.set(answer); done.countDown(); }
                    @Override public void onError(DnsResolver.DnsException error) { done.countDown(); }
                });
        if (!done.await(RESOLVER_TIMEOUT_MS, TimeUnit.MILLISECONDS)) { cancel.cancel(); return null; }
        byte[] answer = result.get();
        return answer == null || answer.length == 0 || answer.length > MAX_PACKET ? null : answer;
    }

    private static void closeQuietly(Object closeable) {
        try {
            if (closeable instanceof java.io.Closeable) ((java.io.Closeable) closeable).close();
            else if (closeable instanceof FileDescriptor) Os.close((FileDescriptor) closeable);
        } catch (Exception ignored) { }
    }

    private DnsForwarder() {}
}
