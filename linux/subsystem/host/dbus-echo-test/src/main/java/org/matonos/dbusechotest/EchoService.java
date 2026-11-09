package org.matonos.dbusechotest;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructMsghdr;
import android.util.Log;

import org.matonos.linux.IDBusService;

import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.HashMap;
import java.util.Map;

/** Small D-Bus provider fixture for exercising bridge traffic in both directions. */
public final class EchoService extends Service {
    private static final String TAG = "DBusEchoTest";
    private static final String NAME = "org.matonos.test.Echo";
    private static final String PATH = "/org/matonos/test/Echo";
    private static final String INTERFACE = "org.matonos.test.Echo";

    @Override public IBinder onBind(Intent intent) {
        return new IDBusService.Stub() {
            @Override public void attach(ParcelFileDescriptor socket) {
                final int uid = Binder.getCallingUid();
                Thread reader = new Thread(() -> serve(socket, uid), "dbus-echo-" + uid);
                reader.start();
            }
        };
    }

    private static void serve(ParcelFileDescriptor socket, int uid) {
        Map<Integer, Integer> callbacks = new HashMap<>();
        int nextSerial = 1;
        try (ParcelFileDescriptor owned = socket) {
            FileOutputStream output = new FileOutputStream(owned.getFileDescriptor());
            while (true) {
                byte[] packet = readPacket(owned.getFileDescriptor());
                if (packet == null) return;
                PortalWire.Message call = PortalWire.decode(packet);
                if (call.type == 2 || call.type == 3) {
                    Integer requestSerial = callbacks.remove(call.replySerial);
                    if (requestSerial != null) {
                        PortalWire.Message result;
                        if (call.type == 3) {
                            result = response(3, requestSerial, call.error, "s", call.body.isEmpty() ? "D-Bus Peer.Ping failed" : call.body.get(0));
                        } else {
                            result = response(2, requestSerial, null, "s", "ok");
                        }
                        write(output, result);
                    }
                    continue;
                }
                if (call.type != 1 || !NAME.equals(call.destination)) continue;
                if (PATH.equals(call.path) && INTERFACE.equals(call.iface) && "Echo".equals(call.member)) {
                    if (!"s".equals(call.signature) || call.body.size() != 1) {
                        write(output, error(call, "org.freedesktop.DBus.Error.InvalidArgs", "Echo expects one string"));
                    } else {
                        write(output, response(call, "s", call.body.get(0)));
                    }
                } else if (PATH.equals(call.path) && INTERFACE.equals(call.iface) && "Ping".equals(call.member)) {
                    if (!call.signature.isEmpty() || !call.body.isEmpty()) {
                        write(output, error(call, "org.freedesktop.DBus.Error.InvalidArgs", "Ping expects no arguments"));
                    } else {
                        PortalWire.Message signal = new PortalWire.Message();
                        signal.type = 4;
                        signal.path = PATH;
                        signal.iface = INTERFACE;
                        signal.member = "Pinged";
                        signal.signature = "s";
                        signal.body.add(Integer.toString(uid));
                        write(output, signal);
                        write(output, response(call, "", null));
                    }
                } else if (PATH.equals(call.path) && INTERFACE.equals(call.iface) && "CallBack".equals(call.member)) {
                    if (!"s".equals(call.signature) || call.body.size() != 1) {
                        write(output, error(call, "org.freedesktop.DBus.Error.InvalidArgs", "CallBack expects one unique name"));
                    } else {
                        int serial = nextSerial++;
                        callbacks.put(serial, call.serial);
                        PortalWire.Message peerCall = new PortalWire.Message();
                        peerCall.type = 1;
                        peerCall.serial = serial;
                        peerCall.path = "/";
                        peerCall.iface = "org.freedesktop.DBus.Peer";
                        peerCall.member = "Ping";
                        peerCall.destination = (String) call.body.get(0);
                        write(output, peerCall);
                    }
                } else {
                    write(output, error(call, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown Echo method"));
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "provider uid=" + uid + " bridge socket failed", e);
        }
    }

    /** Read one whole seqpacket without imposing a userspace message-size limit. */
    private static byte[] readPacket(java.io.FileDescriptor fd) throws Exception {
        ByteBuffer probe = ByteBuffer.allocateDirect(1);
        StructMsghdr peek = new StructMsghdr(null, new ByteBuffer[] { probe }, null, 0);
        int size = Os.recvmsg(fd, peek, OsConstants.MSG_PEEK | OsConstants.MSG_TRUNC);
        if (size == 0) return null;
        if (size < 0) throw new IOException("recvmsg returned a negative packet size");
        ByteBuffer packet = ByteBuffer.allocateDirect(size);
        StructMsghdr read = new StructMsghdr(null, new ByteBuffer[] { packet }, null, 0);
        int received = Os.recvmsg(fd, read, 0);
        if (received != size) throw new IOException("seqpacket size changed: peek=" + size + " read=" + received);
        byte[] bytes = new byte[received];
        packet.position(0);
        packet.get(bytes);
        return bytes;
    }

    private static PortalWire.Message response(PortalWire.Message request, String signature, Object value) {
        return response(2, request.serial, null, signature, value);
    }

    private static PortalWire.Message response(int type, int replySerial, String error, String signature, Object value) {
        PortalWire.Message message = new PortalWire.Message();
        message.type = type;
        message.replySerial = replySerial;
        message.error = error;
        message.signature = signature;
        if (value != null) message.body.add(value);
        return message;
    }

    private static PortalWire.Message error(PortalWire.Message request, String name, String text) {
        return response(3, request.serial, name, "s", text);
    }

    private static void write(FileOutputStream output, PortalWire.Message message) throws IOException {
        output.write(PortalWire.encode(message));
    }
}
