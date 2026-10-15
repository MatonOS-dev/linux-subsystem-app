package org.matonos.linux;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.List;

/**
 * Writes little-endian D-Bus values. Explicit per-type switch, no reflection.
 *
 * <p>Values use plain Java types: {@code Integer} for y/n/q/i/u/h, {@code Boolean}
 * for b, {@code Long} for x/t, {@code Double} for d, {@code String} for s/o/g,
 * {@code List} for arrays and structs, and {@link PortalWire.Variant} for v.
 */
final class DBusWriter {
    private byte[] data = new byte[256];
    private int size;

    int size() { return size; }

    byte[] toBytes() { return Arrays.copyOf(data, size); }

    void u8(int value) {
        need(1);
        data[size++] = (byte) value;
    }

    void align(int alignment) {
        int pad = (alignment - (size % alignment)) % alignment;
        need(pad);
        size += pad;
    }

    private void i16(int value) {
        align(2);
        need(2);
        data[size++] = (byte) value;
        data[size++] = (byte) (value >> 8);
    }

    void i32(int value) {
        align(4);
        need(4);
        for (int i = 0; i < 4; i++) data[size++] = (byte) (value >> (8 * i));
    }

    private void i64(long value) {
        align(8);
        need(8);
        for (int i = 0; i < 8; i++) data[size++] = (byte) (value >> (8 * i));
    }

    private void bytes(byte[] value) {
        need(value.length);
        System.arraycopy(value, 0, data, size, value.length);
        size += value.length;
    }

    private void string(String value) {
        byte[] utf8 = value.getBytes(StandardCharsets.UTF_8);
        i32(utf8.length);
        bytes(utf8);
        u8(0);
    }

    private void signature(String value) {
        DBusSignature.split(value);
        byte[] ascii = value.getBytes(StandardCharsets.US_ASCII);
        u8(ascii.length);
        bytes(ascii);
        u8(0);
    }

    /** Overwrite the little-endian uint32 that starts at {@code at}. */
    void patchU32(int at, int value) {
        for (int i = 0; i < 4; i++) data[at + i] = (byte) (value >> (8 * i));
    }

    void variant(String signature, Object value) {
        DBusSignature.single(signature);
        signature(signature);
        value(signature, value);
    }

    void value(String signature, Object value) {
        DBusSignature.valueType(signature);
        char type = signature.charAt(0);
        switch (type) {
            case 'y': u8(((Number) value).intValue()); return;
            case 'b': i32(((Boolean) value) ? 1 : 0); return;
            case 'n': case 'q': i16(((Number) value).intValue()); return;
            case 'i': case 'u': case 'h': i32(((Number) value).intValue()); return;
            case 'x': case 't': i64(((Number) value).longValue()); return;
            case 'd': i64(Double.doubleToLongBits(((Number) value).doubleValue())); return;
            case 's': case 'o': string((String) value); return;
            case 'g': signature((String) value); return;
            case 'v': {
                PortalWire.Variant variant = (PortalWire.Variant) value;
                variant(variant.signature, variant.value);
                return;
            }
            case 'a': writeArray(signature.substring(1), (List<?>) value); return;
            case '(': case '{': writeStruct(signature, (List<?>) value); return;
            default: throw new IllegalArgumentException("Unsupported D-Bus type: " + type);
        }
    }

    private void writeArray(String element, List<?> values) {
        if (element.isEmpty())
            throw new IllegalArgumentException("Array without element type");
        align(4);
        int lengthAt = size;
        i32(0);
        align(DBusSignature.alignment(element.charAt(0)));
        int start = size;
        for (Object value : values) value(element, value);
        patchU32(lengthAt, size - start);
    }

    private void writeStruct(String signature, List<?> values) {
        align(8);
        char close = signature.charAt(0) == '(' ? ')' : '}';
        int at = 1;
        int index = 0;
        while (signature.charAt(at) != close) {
            int end = DBusSignature.end(signature, at);
            value(signature.substring(at, end), values.get(index++));
            at = end;
        }
    }

    private void need(int extra) {
        if (size + extra > data.length)
            data = Arrays.copyOf(data, Math.max(data.length * 2, size + extra));
    }
}
