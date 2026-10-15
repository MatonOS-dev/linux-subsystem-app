package org.matonos.linux;

import java.util.ArrayList;
import java.util.List;

/**
 * Parses D-Bus type signatures. Plain recursive descent, one explicit switch:
 * no reflection and no annotations.
 */
final class DBusSignature {
    private DBusSignature() { }

    /** Index just past the complete type that starts at {@code pos}. */
    static int end(String signature, int pos) {
        return end(signature, pos, 0, 0, pos > 0 && signature.charAt(pos - 1) == 'a');
    }

    private static int end(String signature, int pos, int arrays, int structs, boolean dictionary) {
        if (signature.length() > 255 || arrays > 32 || structs > 32 || pos < 0 || pos >= signature.length())
            throw new IllegalArgumentException("Invalid D-Bus signature");
        char c = signature.charAt(pos++);
        if (c == 'a') return end(signature, pos, arrays + 1, structs, true);
        if (c == '(' || c == '{') {
            if (c == '{' && !dictionary)
                throw new IllegalArgumentException("Dictionary entry outside array");
            char close = c == '(' ? ')' : '}';
            int count = 0;
            while (pos < signature.length() && signature.charAt(pos) != close) {
                if (c == '{' && count == 0 && "ybnqiuxtdsogh".indexOf(signature.charAt(pos)) < 0)
                    throw new IllegalArgumentException("Invalid dictionary key");
                pos = end(signature, pos, arrays, structs + 1, false);
                count++;
            }
            if (pos >= signature.length() || count == 0 || (c == '{' && count != 2))
                throw new IllegalArgumentException("Invalid D-Bus container signature");
            return pos + 1;
        }
        if ("ybnqiuxtdsoghv".indexOf(c) < 0)
            throw new IllegalArgumentException("Invalid D-Bus type");
        return pos;
    }

    static void valueType(String signature) {
        if (end(signature, 0, 0, 0, true) != signature.length())
            throw new IllegalArgumentException("Expected one complete D-Bus type");
    }

    static void single(String signature) {
        if (end(signature, 0) != signature.length())
            throw new IllegalArgumentException("Expected one complete D-Bus type");
    }

    /** Split a signature into its top-level complete types. */
    static List<String> split(String signature) {
        if (signature == null || signature.length() > 255)
            throw new IllegalArgumentException("Invalid D-Bus signature");
        List<String> out = new ArrayList<>();
        for (int pos = 0; pos < signature.length(); ) {
            int end = end(signature, pos);
            out.add(signature.substring(pos, end));
            pos = end;
        }
        return out;
    }

    /** Byte alignment of the first byte of a complete type. */
    static int alignment(char type) {
        switch (type) {
            case 'y': case 'g': case 'v': return 1;
            case 'n': case 'q': return 2;
            case 'x': case 't': case 'd': case '(': case '{': return 8;
            default: return 4; // b, i, u, h, s, o, a
        }
    }
}
