package org.matonos.linux.stub;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.CompletableFuture;

public final class MachineIdTest {
    public static void main(String[] args) throws Exception {
        File files = new File(args[0]);
        File id = MachineId.ensure(files);
        String first = new String(Files.readAllBytes(id.toPath()), StandardCharsets.US_ASCII);
        if (!first.matches("[0-9a-f]{32}\\n")) throw new AssertionError(first);
        CompletableFuture<?>[] readers = new CompletableFuture<?>[Runtime.getRuntime().availableProcessors()];
        for (int i = 0; i < readers.length; i++) readers[i] = CompletableFuture.runAsync(() -> {
            try {
                File again = MachineId.ensure(files);
                if (!first.equals(new String(Files.readAllBytes(again.toPath()), StandardCharsets.US_ASCII)))
                    throw new AssertionError("Identity changed across launches");
            } catch (Exception e) { throw new RuntimeException(e); }
        });
        CompletableFuture.allOf(readers).join();
        Files.write(id.toPath(), new byte[0]);
        if (MachineId.ensure(files).length() != 33) throw new AssertionError("Empty identity not repaired");
        System.out.println("PASS stable per-install machine-id, concurrent launch, empty-file repair");
    }
}
