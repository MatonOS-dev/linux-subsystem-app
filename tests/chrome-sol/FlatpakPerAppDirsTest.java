import org.matonos.linux.FlatpakPerAppDirs;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.attribute.PosixFilePermissions;

public final class FlatpakPerAppDirsTest {
    public static void main(String[] args) throws Exception {
        File runtime = new File(args[0]);
        File marker = FlatpakPerAppDirs.ensureRef(runtime, "app/com.google.Chrome/x86_64/stable");
        if (!marker.toPath().equals(runtime.toPath().resolve(".flatpak/com.google.Chrome/.ref")))
            throw new AssertionError("wrong Flatpak lock path: " + marker);
        if (!Files.isRegularFile(marker.toPath(), LinkOption.NOFOLLOW_LINKS) || marker.length() != 0)
            throw new AssertionError("per-app ref must be an empty regular lock file");
        if (!Files.getPosixFilePermissions(marker.toPath()).equals(
                PosixFilePermissions.fromString("rw-------")))
            throw new AssertionError("per-app ref must be mode 0600");
        if (!Files.getPosixFilePermissions(marker.toPath().getParent()).equals(
                PosixFilePermissions.fromString("rwx------")))
            throw new AssertionError("per-app runtime directory must be mode 0700");
        if (!FlatpakPerAppDirs.ensureRef(runtime, "app/com.google.Chrome/x86_64/stable").equals(marker))
            throw new AssertionError("repeated launch changed the lock path");

        try {
            FlatpakPerAppDirs.ensureRef(runtime, "app/../escape/x86_64/stable");
            throw new AssertionError("accepted invalid app ref");
        } catch (java.io.IOException expected) { }
        System.out.println(marker.getAbsolutePath());
        System.out.println("PASS stock Flatpak per-app lock ref is present and stable");
    }
}
