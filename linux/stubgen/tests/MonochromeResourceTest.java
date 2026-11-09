package org.matonos.linuxhost.stubgen;

import java.nio.charset.StandardCharsets;

/** Verifies generated launcher drawable resources include the symbolic layer only when available. */
public final class MonochromeResourceTest {
    public static void main(String[] args) throws Exception {
        byte[] withMonochrome = StubGenerator.launcherAdaptiveIconResource(true);
        byte[] withoutMonochrome = StubGenerator.launcherAdaptiveIconResource(false);
        byte[] tableWithMonochrome = StubGenerator.launcherIconResourceTable(true);
        byte[] tableWithoutMonochrome = StubGenerator.launcherIconResourceTable(false);
        if (!contains(withMonochrome, "monochrome")) {
            throw new AssertionError("adaptive icon with symbolic art has no monochrome layer");
        }
        if (contains(withoutMonochrome, "monochrome")) {
            throw new AssertionError("adaptive icon without symbolic art contains a monochrome layer");
        }
        if (!contains(tableWithMonochrome, "res/drawable/monochrome.png")) {
            throw new AssertionError("resource table with symbolic art has no monochrome bitmap");
        }
        if (contains(tableWithoutMonochrome, "res/drawable/monochrome.png")) {
            throw new AssertionError("resource table without symbolic art contains a monochrome bitmap");
        }
    }

    private static boolean contains(byte[] bytes, String value) {
        byte[] needle = value.getBytes(StandardCharsets.UTF_8);
        outer:
        for (int start = 0; start <= bytes.length - needle.length; start++) {
            for (int offset = 0; offset < needle.length; offset++) {
                if (bytes[start + offset] != needle[offset]) {
                    continue outer;
                }
            }
            return true;
        }
        return false;
    }
}
