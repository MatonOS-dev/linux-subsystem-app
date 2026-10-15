package org.matonos.linux;

import java.io.EOFException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

import static org.matonos.linux.PortalWire.*;

/**
 * Host JVM test: a real {@link PeerConnection} over a loopback socket, with the
 * portal semantics from {@link PortalBackend}. No Android and no dbus-java.
 */
public final class PortalConnectionTest {
    static void check(boolean ok) { if (!ok) throw new AssertionError(); }
    static int serial;

    static Message methodCall(String owner, String iface, String method, String signature, Object... args) {
        Message m = new Message();
        m.type = 1; m.serial = ++serial; m.sender = owner; m.destination = "org.freedesktop.portal.Desktop";
        m.path = PortalBackend.DESKTOP; m.iface = "org.freedesktop.portal." + iface; m.member = method;
        m.signature = signature; m.body = Arrays.asList(args);
        return m;
    }

    static List<Object> opts(String token) {
        return dictionary(Collections.singletonMap("handle_token", new Variant("s", token)));
    }

    static Message readMessage(InputStream in) throws Exception {
        byte[] header = new byte[8];
        readFully(in, header);
        int[] frame = validateFrameHeader(header);
        byte[] message = new byte[frame[0]];
        readFully(in, message);
        check(frame[1] == 0);
        return decode(message);
    }

    static void writeMessage(OutputStream out, Message m) throws Exception {
        byte[] bytes = encode(m);
        out.write(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(bytes.length).putInt(0).array());
        out.write(bytes);
        out.flush();
    }

    static void readFully(InputStream in, byte[] buffer) throws Exception {
        int offset = 0;
        while (offset < buffer.length) {
            int read = in.read(buffer, offset, buffer.length - offset);
            if (read < 0) throw new EOFException();
            offset += read;
        }
    }

    public static void main(String[] args) throws Exception {
        PortalTestPeer.Platform platform = new PortalTestPeer.Platform();
        PortalBackend backend = new PortalBackend(platform);
        final boolean[] clientClosed = new boolean[1];

        try {
            ServerSocket probe = new ServerSocket(0, 1, InetAddress.getLoopbackAddress());
            probe.close();
        } catch (java.net.SocketException unavailable) {
            System.out.println("SKIP: loopback sockets unavailable: " + unavailable.getMessage());
            return;
        }

        ServerSocket listener = new ServerSocket(0, 1, InetAddress.getLoopbackAddress());
        Socket client = new Socket(InetAddress.getLoopbackAddress(), listener.getLocalPort());
        Socket accepted = listener.accept();
        listener.close();

        PeerConnection connection = new PeerConnection(
                new PipeChannel(accepted.getInputStream(), accepted.getOutputStream()),
                new PeerConnection.Handler() {
                    @Override public List<Message> methodCall(Message call) { return backend.dispatch(call); }
                    @Override public void signal(Message message) {
                        if ("ClientClosed".equals(message.member)) clientClosed[0] = true;
                    }
                });
        Thread server = new Thread(connection::serve, "test-peer");
        server.start();

        InputStream in = client.getInputStream();
        OutputStream out = client.getOutputStream();

        // Settings.Read returns a variant wrapping uint 1.
        writeMessage(out, methodCall(":1.1", "Settings", "Read", "ss", "org.freedesktop.appearance", "color-scheme"));
        Message read = readMessage(in);
        check(read.type == 2 && "v".equals(read.signature));
        Variant outer = (Variant) read.body.get(0);
        check("v".equals(outer.signature) && Integer.valueOf(1).equals(((Variant) outer.value).value));

        // An unknown interface is a D-Bus error, not a crash.
        writeMessage(out, methodCall(":1.1", "Bogus", "Nope", ""));
        Message bad = readMessage(in);
        check(bad.type == 3 && "org.freedesktop.DBus.Error.UnknownMethod".equals(bad.error));

        // Inhibit replies with an object path, then emits a Response signal.
        writeMessage(out, methodCall(":1.1", "Inhibit", "Inhibit", "sua{sv}", "", 8, opts("first")));
        Message inhibit = readMessage(in);
        check(inhibit.type == 2 && "o".equals(inhibit.signature));
        Message response = readMessage(in);
        check(response.type == 4 && "Response".equals(response.member) && "ua{sv}".equals(response.signature));

        // An inbound signal is routed to the handler.
        Message closed = new Message();
        closed.type = 4; closed.serial = 99; closed.path = "/org/matonos/PortalBackend";
        closed.iface = "org.matonos.PortalBackend"; closed.member = "ClientClosed";
        closed.signature = "s"; closed.body = Collections.singletonList(":1.1");
        writeMessage(out, closed);
        for (int i = 0; i < 200 && !clientClosed[0]; i++) Thread.sleep(10);
        check(clientClosed[0]);

        client.close();
        server.join(2000);
        System.out.println("PASS: PeerConnection host integration (method call, error, signal, inbound signal)");
    }
}
