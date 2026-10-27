package org.matonos.linux.stub;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import org.junit.Test;
import static org.junit.Assert.assertEquals;

public final class StubPasswdTest {
    @Test public void mapsTheStubUidToItsRealHome() throws Exception {
        Path dir=Files.createTempDirectory("stub-passwd-");
        try {
            Path home=Files.createDirectory(dir.resolve("stub-home"));
            Path passwd=dir.resolve("passwd");
            Path group=dir.resolve("group");
            StubPasswd.write(passwd.toFile(),group.toFile(),10137,10137,home.toFile());
            String expected="matonos10137:x:10137:10137:MatonOS Linux:"+home.toRealPath()+":/system/bin/sh\n";
            String actual=new String(Files.readAllBytes(passwd),StandardCharsets.UTF_8);
            assertEquals(expected,actual);
            assertEquals("matonos10137:x:10137:\n",new String(Files.readAllBytes(group),StandardCharsets.UTF_8));
        } finally {
            Files.deleteIfExists(dir.resolve("passwd"));
            Files.deleteIfExists(dir.resolve("group"));
            Files.deleteIfExists(dir.resolve("stub-home"));
            Files.deleteIfExists(dir);
        }
    }
}
