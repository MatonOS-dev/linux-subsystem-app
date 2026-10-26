package org.matonos.linux.stub;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermission;
import java.util.EnumSet;

/** Host-side coverage for the isolated process's sanitized extra-data tar stream. */
public final class TarStreamHostTest {
    public static void main(String[] args) throws Exception {
        Path root=Files.createTempDirectory("maton-extra-tar");
        try {
            Path extra=Files.createDirectories(root.resolve("source/extra/lib"));
            Path binary=extra.resolve("chrome");Files.write(binary,"payload".getBytes(StandardCharsets.UTF_8));
            Files.setPosixFilePermissions(binary,EnumSet.of(PosixFilePermission.OWNER_READ,
                    PosixFilePermission.OWNER_WRITE,PosixFilePermission.OWNER_EXECUTE));
            Files.createSymbolicLink(extra.resolve("current"),Path.of("lib/chrome"));
            Files.createSymbolicLink(extra.resolve("absolute"),Path.of("/app/extra/lib/chrome"));
            Files.createSymbolicLink(extra.resolve("parent-link"),Path.of("../../outside"));
            ByteArrayOutputStream tar=new ByteArrayOutputStream();TarStream.writeExtra(root.resolve("source/extra"),tar);
            Path target=Files.createDirectories(root.resolve("target"));TarStream.extractExtra(
                    new ByteArrayInputStream(tar.toByteArray()),target);
            Path received=target.resolve("extra");
            if(!"payload".equals(Files.readString(received.resolve("lib/chrome"))))throw new AssertionError("file payload did not round trip");
            if(!Files.isExecutable(received.resolve("lib/chrome")))throw new AssertionError("executable permission was lost");
            if(!Files.readSymbolicLink(received.resolve("lib/current")).toString().equals("lib/chrome"))throw new AssertionError("relative symlink did not round trip");
            if(!Files.readSymbolicLink(received.resolve("lib/absolute")).toString().equals("/app/extra/lib/chrome"))throw new AssertionError("absolute symlink target changed");
            if(!Files.readSymbolicLink(received.resolve("lib/parent-link")).toString().equals("../../outside"))throw new AssertionError("relative parent symlink target changed");
            Path install=Files.createDirectories(root.resolve("install/app/files"));
            Files.writeString(install.resolve("deploy.txt"),"deployment payload");
            Path staging=install.getParent().getParent();
            Files.createDirectories(staging.resolve("repo/objects"));Files.writeString(staging.resolve("repo/objects/object"),"repo payload");
            Files.writeString(staging.resolve(".v3-store-root"),"private marker");
            Files.createDirectories(staging.resolve("isolated-repo"));
            Path archive=root.resolve("deployment.tar");
            try(java.io.OutputStream out=Files.newOutputStream(archive)) {
                TarStream.writeDeployment(staging,staging.resolve("repo"),out);
            }
            Path unpack=Files.createDirectories(root.resolve("unpacked"));
            Process untar=new ProcessBuilder("tar","-xf",archive.toString(),"-C",unpack.toString()).inheritIO().start();
            if(untar.waitFor()!=0)throw new AssertionError("deployment tar could not be extracted");
            if(!Files.readString(unpack.resolve("install/app/files/deploy.txt")).equals("deployment payload"))throw new AssertionError("deployment tree was not streamed");
            if(!Files.readString(unpack.resolve("repo/objects/object")).equals("repo payload"))throw new AssertionError("repo tree was not streamed");
            if(Files.exists(unpack.resolve("install/.v3-store-root"))||Files.exists(unpack.resolve("install/isolated-repo")))throw new AssertionError("private staging data entered the archive");
            System.out.println("PASS isolated apply_extra tar tree and traversal checks");
        } finally {
            try(java.util.stream.Stream<Path> paths=Files.walk(root)) {paths.sorted(java.util.Comparator.reverseOrder()).forEach(path->{try{Files.deleteIfExists(path);}catch(Exception ignored){}});}
        }
    }
}
