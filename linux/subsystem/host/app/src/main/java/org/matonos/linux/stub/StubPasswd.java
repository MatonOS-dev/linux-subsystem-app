package org.matonos.linux.stub;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Host-side passwd view for the Android app UID while Flatpak sets up bwrap. */
final class StubPasswd {
    static void write(File passwdFile, File groupFile, int uid, int gid, File home) throws IOException {
        String directory = home.getCanonicalPath();
        if (uid < 0 || gid < 0 || directory.indexOf(':') >= 0 || directory.indexOf('\n') >= 0)
            throw new IOException("Invalid stub account identity");
        String name = "matonos" + uid;
        String passwd = name + ":x:" + uid + ":" + gid + ":MatonOS Linux:" + directory + ":/system/bin/sh\n";
        Files.write(passwdFile.toPath(), passwd.getBytes(StandardCharsets.UTF_8));
        String group = name + ":x:" + gid + ":\n";
        Files.write(groupFile.toPath(), group.getBytes(StandardCharsets.UTF_8));
    }

    private StubPasswd() { }
}
