package org.matonos.linux;

import java.nio.charset.StandardCharsets;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.util.ArrayList;
import java.util.List;

/**
 * Reads little- or big-endian D-Bus values with bounds checks. Explicit
 * per-type switch, no reflection.
 */
final class DBusReader {
    private static final int MAX_DEPTH = 64;

    private final byte[] data;
    private int pos;
    private int limit;
    boolean littleEndian = true;

    DBusReader(byte[] data) { this.data = data; this.limit = data.length; }

    void limit(int next) {
        if (next < pos || next > data.length) throw new IllegalArgumentException("Invalid read limit");
        limit = next;
    }

    int position() { return pos; }

    void position(int next) {
        if (next < 0 || next > limit)
            throw new IllegalArgumentException("D-Bus read out of bounds");
        pos = next;
    }

    void align(int alignment) {
        int pad = (alignment - (pos % alignment)) % alignment;
        check(pad);
        for (int i = 0; i < pad; i++)
            if (data[pos++] != 0) throw new IllegalArgumentException("Nonzero D-Bus padding");
    }

    int u8() {
        check(1);
        return data[pos++] & 255;
    }

    int i16() {
        align(2);
        check(2);
        int first = data[pos] & 255, second = data[pos + 1] & 255;
        pos += 2;
        return littleEndian ? (short) (first | (second << 8)) : (short) ((first << 8) | second);
    }

    long u32() {
        align(4);
        check(4);
        long value = 0;
        if (littleEndian) for (int i = 3; i >= 0; i--) value = (value << 8) | (data[pos + i] & 255);
        else for (int i = 0; i < 4; i++) value = (value << 8) | (data[pos + i] & 255);
        pos += 4;
        return value;
    }

    int i32() { return (int) u32(); }

    long i64() {
        align(8);
        check(8);
        long value = 0;
        if (littleEndian) for (int i = 7; i >= 0; i--) value = (value << 8) | (data[pos + i] & 255);
        else for (int i = 0; i < 8; i++) value = (value << 8) | (data[pos + i] & 255);
        pos += 8;
        return value;
    }

    private double f64() { return Double.longBitsToDouble(i64()); }

    private String string() {
        align(4);
        long length = u32();
        if (length > data.length) throw new IllegalArgumentException("Invalid D-Bus string length");
        int n = (int) length;
        check(n + 1);
        String value;
        try {
            value = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(data, pos, n)).toString();
        } catch (CharacterCodingException error) {
            throw new IllegalArgumentException("Invalid D-Bus UTF-8", error);
        }
        if (value.indexOf('\0') >= 0) throw new IllegalArgumentException("Embedded string terminator");
        pos += n;
        if (data[pos++] != 0) throw new IllegalArgumentException("D-Bus string missing terminator");
        return value;
    }

    String signature() {
        int n = u8();
        check(n + 1);
        String value = new String(data, pos, n, StandardCharsets.US_ASCII);
        pos += n;
        if (data[pos++] != 0) throw new IllegalArgumentException("D-Bus signature missing terminator");
        DBusSignature.split(value);
        return value;
    }

    Object value(String signature, int depth) {
        if (depth > MAX_DEPTH)
            throw new IllegalArgumentException("D-Bus value too deeply nested");
        DBusSignature.valueType(signature);
        char type = signature.charAt(0);
        switch (type) {
            case 'y': return u8();
            case 'b': {
                int booleanValue = i32();
                if (booleanValue != 0 && booleanValue != 1) throw new IllegalArgumentException("Invalid D-Bus boolean");
                return booleanValue == 1;
            }
            case 'n': return i16();
            case 'q': return i16() & 65535;
            case 'i': case 'u': case 'h': return i32();
            case 'x': case 't': return i64();
            case 'd': return f64();
            case 's': case 'o': return string();
            case 'g': return signature();
            case 'v': {
                String inner = signature();
                DBusSignature.single(inner);
                return new PortalWire.Variant(inner, value(inner, depth + 1));
            }
            case 'a': return readArray(signature.substring(1), depth);
            case '(': case '{': return readStruct(signature, depth);
            default: throw new IllegalArgumentException("Unsupported D-Bus type: " + type);
        }
    }

    private List<Object> readArray(String element, int depth) {
        if (element.isEmpty())
            throw new IllegalArgumentException("Array without element type");
        align(4);
        long length = u32();
        if (length > data.length) throw new IllegalArgumentException("Invalid D-Bus array length");
        align(DBusSignature.alignment(element.charAt(0)));
        int end = pos + (int) length;
        if (end < pos || end > limit) throw new IllegalArgumentException("D-Bus array overruns message");
        List<Object> out = new ArrayList<>();
        int outerLimit = limit;
        limit = end;
        try {
            while (pos < end) {
                int start = pos;
                out.add(value(element, depth + 1));
                if (pos <= start) throw new IllegalArgumentException("D-Bus array made no progress");
            }
        } finally {
            limit = outerLimit;
        }
        if (pos != end) throw new IllegalArgumentException("D-Bus array length mismatch");
        return out;
    }

    private List<Object> readStruct(String signature, int depth) {
        align(8);
        char close = signature.charAt(0) == '(' ? ')' : '}';
        List<Object> out = new ArrayList<>();
        int at = 1;
        while (signature.charAt(at) != close) {
            int end = DBusSignature.end(signature, at);
            out.add(value(signature.substring(at, end), depth + 1));
            at = end;
        }
        return out;
    }

    private void check(int n) {
        if (n < 0 || n > limit - pos)
            throw new IllegalArgumentException("Truncated D-Bus message");
    }
}
