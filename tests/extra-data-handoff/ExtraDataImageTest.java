package org.matonos.linux.stub;

import org.matonos.linuxhost.stubgen.ErofsImage;
import org.matonos.linuxhost.stubgen.ObjectPacks;
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.List;

/** The returned stub image carries extra data only; publication must contain the full app tree. */
public final class ExtraDataImageTest {
    public static void main(String[] args)throws Exception {
        Path root=Files.createTempDirectory("extra-data-image-");
        try {
            Path deployed=root.resolve("deployment/files");
            Files.createDirectories(deployed.resolve("bin"));
            Files.writeString(deployed.resolve("bin/chrome"),"#!/app/bin/sh\nexec /app/extra/chrome\n");
            Files.setPosixFilePermissions(deployed.resolve("bin/chrome"),PosixFilePermissions.fromString("rwxr-xr-x"));

            Path returned=root.resolve("returned/app/extra");
            Files.createDirectories(returned);
            Files.writeString(returned.resolve("chrome"),"chrome executable payload\n");
            Files.setPosixFilePermissions(returned.resolve("chrome"),PosixFilePermissions.fromString("rwxr-xr-x"));
            ExtraDataImage.mergeExtra(root.resolve("returned").toFile(),deployed.toFile());
            if(Files.exists(root.resolve("returned/app/extra/chrome")))
                throw new AssertionError("extra-data payload was not moved into deployment");

            File fakeMkfs=root.resolve("fake-mkfs-erofs").toFile();
            Files.writeString(fakeMkfs.toPath(),"#!/bin/sh\ncat \"${11}\" > \"${10}\"\n");
            if(!fakeMkfs.setExecutable(true))throw new AssertionError("Cannot make fake mkfs executable");
            File imageOut=root.resolve("published-image").toFile();
            List<ObjectPacks.Entry> entries=ObjectPacks.fromTreeWithModes(deployed.toFile(),"/app/");
            ErofsImage.build(entries,imageOut,fakeMkfs,0);
            Path extracted=root.resolve("published");Files.createDirectories(extracted);
            Process tar=new ProcessBuilder("tar","-xf",imageOut.toPath().resolve("deployment.erofs").toString(),"-C",extracted.toString()).start();
            if(tar.waitFor()!=0)throw new AssertionError("Cannot extract published image fixture");
            Path app=extracted.resolve("app");
            if(!Files.readString(app.resolve("bin/chrome")).contains("exec /app/extra/chrome"))
                throw new AssertionError("published image omitted /app/bin/chrome");
            if(!Files.readString(app.resolve("extra/chrome")).contains("chrome executable payload"))
                throw new AssertionError("published image omitted returned /app/extra payload");
            System.out.println("PASS extra-data handback is merged into the full published app EROFS image");
        } finally {
            try(var paths=Files.walk(root)) {
                paths.sorted(java.util.Comparator.reverseOrder()).forEach(path->{try{Files.deleteIfExists(path);}catch(Exception ignored){}});
            }
        }
    }
}
