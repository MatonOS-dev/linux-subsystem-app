package org.matonos.linux;

import java.io.FileDescriptor;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The portal's D-Bus wire codec and message model. Pure Java: it has no
 * dbus-java types, no annotations and no reflection. The MBP1 frame header
 * (eight bytes: little-endian payload length and descriptor count) is validated
 * here; framing and descriptor passing live in the transport.
 */
final class PortalWire {
    static final int MAX_MESSAGE = 1024 * 1024, MAX_FDS = 16;

    /** One (signature, value) pair; the value model is documented on DBusWriter. */
    static final class Variant {
        final String signature;
        final Object value;
        Variant(String signature, Object value) { this.signature = signature; this.value = value; }
    }

    /** A decoded or to-be-encoded D-Bus message. */
    static final class Message {
        int type, flags, serial, replySerial, fdCount;
        String path, iface, member, error, destination, sender, signature = "";
        List<Object> body = new ArrayList<>();
        List<FileDescriptor> fds = Collections.emptyList();
    }

    /** Validate the MBP1 frame header; returns {payloadLength, descriptorCount}. */
    static int[] validateFrameHeader(byte[] header) {
        if (header.length != 8) throw new IllegalArgumentException("Invalid portal frame header");
        ByteBuffer b = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        int size = b.getInt(), fds = b.getInt();
        if (size < 16 || size > MAX_MESSAGE || fds < 0 || fds > MAX_FDS)
            throw new IllegalArgumentException("Malformed portal frame header");
        return new int[] { size, fds };
    }

    static Message decode(byte[] bytes) {
        return decode(bytes, Collections.emptyList());
    }

    static Message decode(byte[] bytes, List<FileDescriptor> descriptors) {
        if (bytes.length < 16 || bytes.length > MAX_MESSAGE)
            throw new IllegalArgumentException("Invalid message size");
        DBusReader reader = new DBusReader(bytes);
        int endianness = reader.u8();
        if (endianness != 'l' && endianness != 'B')
            throw new IllegalArgumentException("Invalid D-Bus endianness");
        reader.littleEndian = endianness == 'l';
        int type = reader.u8(), flags = reader.u8(), version = reader.u8();
        long bodyLength = reader.u32();
        int serial = reader.i32();
        if (version != 1 || type < 1 || type > 4 || serial == 0)
            throw new IllegalArgumentException("Invalid D-Bus header fields");
        long headerLength = reader.u32();
        if (bodyLength > MAX_MESSAGE || headerLength > MAX_MESSAGE)
            throw new IllegalArgumentException("Invalid D-Bus message length");
        long paddedHeader = (headerLength + 7) & ~7L;
        if (16L + paddedHeader + bodyLength != bytes.length)
            throw new IllegalArgumentException("Invalid D-Bus message length");

        Message message = new Message();
        message.type = type;
        message.flags = flags;
        message.serial = serial;
        int fieldsEnd = 16 + (int) headerLength;
        reader.limit(fieldsEnd);
        boolean[] seen = new boolean[256];
        String[] fieldTypes = { "", "o", "s", "s", "s", "u", "s", "s", "g", "u" };
        while (reader.position() < fieldsEnd) {
            reader.align(8);
            if (reader.position() == fieldsEnd) break;
            if (reader.position() > fieldsEnd)
                throw new IllegalArgumentException("Malformed D-Bus header");
            int id = reader.u8();
            if (id == 0 || seen[id]) throw new IllegalArgumentException("Invalid or duplicate D-Bus header field");
            seen[id] = true;
            String fieldSignature = reader.signature();
            DBusSignature.single(fieldSignature);
            if (id < fieldTypes.length && !fieldTypes[id].equals(fieldSignature))
                throw new IllegalArgumentException("Invalid D-Bus header field type");
            Object value = reader.value(fieldSignature, 0);
            switch (id) {
                case 1: message.path = (String) value; break;
                case 2: message.iface = (String) value; break;
                case 3: message.member = (String) value; break;
                case 4: message.error = (String) value; break;
                case 5: message.replySerial = (Integer) value; break;
                case 6: message.destination = (String) value; break;
                case 7: message.sender = (String) value; break;
                case 8: message.signature = (String) value; break;
                case 9: {
                    long count = Integer.toUnsignedLong((Integer) value);
                    if (count > MAX_FDS) throw new IllegalArgumentException("Invalid UNIX_FDS header");
                    message.fdCount = (int) count;
                    break;
                }
                default: break; // unknown header fields are read and ignored
            }
        }
        if ((type == 1 && (message.path == null || message.member == null))
                || (type == 2 && message.replySerial == 0)
                || (type == 3 && (message.error == null || message.replySerial == 0))
                || (type == 4 && (message.path == null || message.iface == null || message.member == null)))
            throw new IllegalArgumentException("Missing required D-Bus header");
        reader.limit(bytes.length);
        reader.align(8);
        for (String part : DBusSignature.split(message.signature))
            message.body.add(reader.value(part, 0));

        if (reader.position() != bytes.length) throw new IllegalArgumentException("Unconsumed D-Bus body");

        if (descriptors == null) descriptors = Collections.emptyList();
        if (descriptors.size() != message.fdCount)
            throw new IllegalArgumentException("D-Bus descriptor count mismatch");
        message.fds = descriptors;
        return message;
    }

    static byte[] encode(Message message) {
        List<String> parts = (message.signature == null || message.signature.isEmpty())
                ? Collections.emptyList() : DBusSignature.split(message.signature);
        if (parts.size() != message.body.size())
            throw new IllegalArgumentException("Signature/body arity mismatch");
        DBusWriter body = new DBusWriter();
        for (int i = 0; i < parts.size(); i++) body.value(parts.get(i), message.body.get(i));
        byte[] bodyBytes = body.toBytes();

        DBusWriter header = new DBusWriter();
        header.u8('l');
        header.u8(message.type);
        header.u8(message.flags);
        header.u8(1);
        header.i32(bodyBytes.length);
        header.i32(message.serial);
        header.i32(0);
        int fieldsAt = header.size();
        if (message.path != null) field(header, 1, "o", message.path);
        if (message.iface != null) field(header, 2, "s", message.iface);
        if (message.member != null) field(header, 3, "s", message.member);
        if (message.error != null) field(header, 4, "s", message.error);
        if (message.replySerial != 0) field(header, 5, "u", message.replySerial);
        if (message.destination != null) field(header, 6, "s", message.destination);
        if (message.sender != null) field(header, 7, "s", message.sender);
        if (message.signature != null && !message.signature.isEmpty()) field(header, 8, "g", message.signature);
        if (message.fdCount != 0) field(header, 9, "u", message.fdCount);
        header.patchU32(12, header.size() - fieldsAt);
        header.align(8);

        byte[] headerBytes = header.toBytes();
        if (headerBytes.length + bodyBytes.length > MAX_MESSAGE)
            throw new IllegalArgumentException("D-Bus message too large");
        byte[] out = new byte[headerBytes.length + bodyBytes.length];
        System.arraycopy(headerBytes, 0, out, 0, headerBytes.length);
        System.arraycopy(bodyBytes, 0, out, headerBytes.length, bodyBytes.length);
        return out;
    }

    private static void field(DBusWriter writer, int id, String signature, Object value) {
        writer.align(8);
        writer.u8(id);
        writer.variant(signature, value);
    }

    static Map<String, Variant> dictionary(Object array) {
        Map<String, Variant> result = new LinkedHashMap<>();
        for (Object entry : (List<?>) array) {
            List<?> pair = (List<?>) entry;
            result.put((String) pair.get(0), (Variant) pair.get(1));
        }
        return result;
    }

    static List<Object> dictionary(Map<String, Variant> values) {
        List<Object> result = new ArrayList<>();
        values.forEach((key, value) -> result.add(java.util.Arrays.asList(key, value)));
        return result;
    }
}
