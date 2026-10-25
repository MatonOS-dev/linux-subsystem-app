package org.matonos.linux.stub;

import java.io.ByteArrayInputStream;
import java.util.Arrays;

public final class FlatpakOutputTest {
    public static void main(String[] args) throws Exception {
        byte[] input = new byte[5 * 1024 * 1024 + 17];
        for (int i = 0; i < input.length; i++) input[i] = (byte) (i * 31);
        byte[] output = FlatpakOutput.readAll(new ByteArrayInputStream(input));
        if (!Arrays.equals(input, output)) throw new AssertionError("large Flatpak output was truncated");
        System.out.println("PASS complete Flatpak output above 4 MiB");
    }
}
