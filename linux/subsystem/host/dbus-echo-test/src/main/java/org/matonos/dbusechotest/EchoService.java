package org.matonos.dbusechotest;

import org.matonos.dbus.DBusMessage;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructMsghdr;
import android.system.StructCmsghdr;
import android.util.Log;

import org.matonos.linux.IDBusService;

import java.io.FileOutputStream;
import java.io.BufferedReader;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashMap;
import java.util.Map;

/** Small D-Bus provider fixture for exercising bridge traffic in both directions. */
public final class EchoService extends Service {
    private static final String TAG = "DBusEchoTest";
    private static final String NAME = "org.matonos.test.Echo";
    private static final String PATH = "/org/matonos/test/Echo";
    private static final String INTERFACE = "org.matonos.test.Echo";
    // Linux UAPI values missing from Android's public OsConstants stubs.
    private static final int LINUX_SCM_RIGHTS = 1;
    private static final int LINUX_MSG_CMSG_CLOEXEC = 0x40000000;

    @Override public IBinder onBind(Intent intent) {
        return new IDBusService.Stub() {
            @Override public void attachWithIcons(ParcelFileDescriptor socket, org.matonos.linux.IStubIcons icons) {
                attach(socket);
            }
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
            Log.i(TAG, "attached by uid=" + uid);
            // Self-test: ask the stub's bus who owns our name; the reply proves the round trip.
            int selfTest = nextSerial++;
            DBusMessage.Message probe = new DBusMessage.Message();
            probe.type = 1; probe.serial = selfTest; probe.path = "/org/freedesktop/DBus";
            probe.iface = "org.freedesktop.DBus"; probe.member = "GetNameOwner";
            probe.destination = "org.freedesktop.DBus"; probe.signature = "s"; probe.body.add(NAME);
            write(output, probe);
            while (true) {
                Packet packet = readPacket(owned.getFileDescriptor());
                if (packet == null) return;
                java.util.ArrayList<java.io.FileDescriptor> descriptors = new java.util.ArrayList<>();
                for (ParcelFileDescriptor descriptor : packet.descriptors) descriptors.add(descriptor.getFileDescriptor());
                DBusMessage.Message call;
                try { call = DBusMessage.decode(packet.bytes, descriptors); }
                catch (Exception e) { closeDescriptors(packet.descriptors); throw e; }
                if ((call.type == 2 || call.type == 3) && call.replySerial == selfTest) {
                    if (call.type == 2) Log.i(TAG, "self-test uid=" + uid + ": " + NAME + " owner=" + call.body);
                    else Log.e(TAG, "self-test uid=" + uid + " failed: " + call.error + " " + call.body);
                    closeDescriptors(packet.descriptors);
                    continue;
                }
                if (call.type == 2 || call.type == 3) {
                    Integer requestSerial = callbacks.remove(call.replySerial);
                    if (requestSerial != null) {
                        DBusMessage.Message result;
                        if (call.type == 3) {
                            result = response(3, requestSerial, call.error, "s", call.body.isEmpty() ? "D-Bus Peer.Ping failed" : call.body.get(0));
                        } else {
                            result = response(2, requestSerial, null, "s", "ok");
                        }
                        write(output, result);
                    }
                    closeDescriptors(packet.descriptors);
                    continue;
                }
                if (call.type != 1 || !NAME.equals(call.destination)) { closeDescriptors(packet.descriptors); continue; }
                if (PATH.equals(call.path) && INTERFACE.equals(call.iface) && "Echo".equals(call.member)) {
                    if (!"s".equals(call.signature) || call.body.size() != 1) {
                        write(output, error(call, "org.freedesktop.DBus.Error.InvalidArgs", "Echo expects one string"));
                    } else {
                        write(output, response(call, "s", call.body.get(0)));
                    }
                } else if (PATH.equals(call.path) && INTERFACE.equals(call.iface) && "EchoFd".equals(call.member)) {
                    if (!"h".equals(call.signature) || call.body.size() != 1 || call.fds.isEmpty()) {
                        write(output, error(call, "org.freedesktop.DBus.Error.InvalidArgs", "EchoFd expects one fd"));
                    } else {
                        int handle = (Integer) call.body.get(0);
                        if (handle < 0 || handle >= call.fds.size()) {
                            write(output, error(call, "org.freedesktop.DBus.Error.InvalidArgs", "EchoFd handle is out of range"));
                        } else {
                            BufferedReader reader = new BufferedReader(new InputStreamReader(new FileInputStream(call.fds.get(handle))));
                            String firstLine = reader.readLine();
                            if (firstLine == null) firstLine = "";
                            write(output, response(call, "s", firstLine));
                        }
                    }
                } else if (PATH.equals(call.path) && INTERFACE.equals(call.iface) && "Ping".equals(call.member)) {
                    if (!call.signature.isEmpty() || !call.body.isEmpty()) {
                        write(output, error(call, "org.freedesktop.DBus.Error.InvalidArgs", "Ping expects no arguments"));
                    } else {
                        DBusMessage.Message signal = new DBusMessage.Message();
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
                        DBusMessage.Message peerCall = new DBusMessage.Message();
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
                closeDescriptors(packet.descriptors);
            }
        } catch (Exception e) {
            Log.e(TAG, "provider uid=" + uid + " bridge socket failed", e);
        }
    }

    /** Read one whole seqpacket without imposing a userspace message-size limit. */
    private static final class Packet {
        final byte[] bytes;
        final java.util.List<ParcelFileDescriptor> descriptors;
        Packet(byte[] bytes, java.util.List<ParcelFileDescriptor> descriptors) { this.bytes = bytes; this.descriptors = descriptors; }
    }

    private static Packet readPacket(java.io.FileDescriptor fd) throws Exception {
        ByteBuffer probe = ByteBuffer.allocateDirect(1);
        // An empty control array keeps MSG_PEEK from installing duplicate SCM_RIGHTS fds.
        StructMsghdr peek = new StructMsghdr(null, new ByteBuffer[] { probe }, new StructCmsghdr[0], 0);
        int size = Os.recvmsg(fd, peek, OsConstants.MSG_PEEK | OsConstants.MSG_TRUNC);
        if (size == 0) return null;
        if (size < 0) throw new IOException("recvmsg returned a negative packet size");
        ByteBuffer packet = ByteBuffer.allocateDirect(size);
        StructMsghdr read = new StructMsghdr(null, new ByteBuffer[] { packet }, null, 0);
        int received = Os.recvmsg(fd, read, LINUX_MSG_CMSG_CLOEXEC);
        if (received != size) throw new IOException("seqpacket size changed: peek=" + size + " read=" + received);
        byte[] bytes = new byte[received];
        packet.position(0);
        packet.get(bytes);
        java.util.ArrayList<ParcelFileDescriptor> descriptors = new java.util.ArrayList<>();
        if (read.msg_control != null) for (StructCmsghdr header : read.msg_control) {
            if (header.cmsg_level != OsConstants.SOL_SOCKET || header.cmsg_type != LINUX_SCM_RIGHTS || header.cmsg_data.length % Integer.BYTES != 0)
                throw new IOException("unexpected bridge ancillary message");
            ByteBuffer values = ByteBuffer.wrap(header.cmsg_data).order(ByteOrder.nativeOrder());
            while (values.remaining() >= Integer.BYTES) descriptors.add(ParcelFileDescriptor.adoptFd(values.getInt()));
        }
        if ((read.msg_flags & OsConstants.MSG_CTRUNC) != 0) {
            closeDescriptors(descriptors);
            throw new IOException("bridge fd ancillary data was truncated");
        }
        return new Packet(bytes, descriptors);
    }

    private static void closeDescriptors(java.util.List<ParcelFileDescriptor> descriptors) {
        for (ParcelFileDescriptor descriptor : descriptors) try { descriptor.close(); }
        catch (IOException e) { Log.e(TAG, "provider descriptor close failed", e); }
    }

    private static DBusMessage.Message response(DBusMessage.Message request, String signature, Object value) {
        return response(2, request.serial, null, signature, value);
    }

    private static DBusMessage.Message response(int type, int replySerial, String error, String signature, Object value) {
        DBusMessage.Message message = new DBusMessage.Message();
        message.type = type;
        message.replySerial = replySerial;
        message.error = error;
        message.signature = signature;
        if (value != null) message.body.add(value);
        return message;
    }

    private static DBusMessage.Message error(DBusMessage.Message request, String name, String text) {
        return response(3, request.serial, name, "s", text);
    }

    private static void write(FileOutputStream output, DBusMessage.Message message) throws IOException {
        output.write(DBusMessage.encode(message));
    }
}
