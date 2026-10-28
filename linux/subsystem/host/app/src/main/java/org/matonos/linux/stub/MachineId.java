package org.matonos.linux.stub;

import java.io.File;
import java.io.IOException;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.UUID;

/** A stable Linux machine identity for this stub installation, outside cache. */
final class MachineId {
    private MachineId() { }

    static synchronized File ensure(File files) throws IOException {
        Path directory = files.toPath();
        Files.createDirectories(directory);
        Path id = directory.resolve("linux-machine-id");
        // Launch and extra-data installation can run in different processes.
        try (FileChannel channel = FileChannel.open(directory.resolve("linux-machine-id.lock"),
                StandardOpenOption.CREATE, StandardOpenOption.WRITE);
                FileLock lock = channel.lock()) {
            if (!lock.isValid()) throw new IOException("Cannot lock Linux machine identity");
            String value = Files.exists(id)
                    ? new String(Files.readAllBytes(id), StandardCharsets.US_ASCII) : "";
            if (!value.matches("[0-9a-f]{32}\\n") || value.startsWith("00000000000000000000000000000000")) {
                value = UUID.randomUUID().toString().replace("-", "") + "\n";
                Path temporary = Files.createTempFile(directory, "machine-id-", ".tmp");
                try {
                    Files.write(temporary, value.getBytes(StandardCharsets.US_ASCII));
                    Files.move(temporary, id, StandardCopyOption.ATOMIC_MOVE,
                            StandardCopyOption.REPLACE_EXISTING);
                } finally {
                    Files.deleteIfExists(temporary);
                }
            }
        }
        return id.toFile();
    }
}
