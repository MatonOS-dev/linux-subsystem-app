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
            List<ObjectPacks.Entry> beforeHandback=appImageEntries(deployed.toFile());

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

            // This models the old order: entries captured before merging the
            // stub's image cannot publish the extra-data files.
            File staleOut=root.resolve("stale-image").toFile();
            ErofsImage.build(beforeHandback,staleOut,fakeMkfs,0);
            Path staleExtracted=root.resolve("stale-extracted");Files.createDirectories(staleExtracted);
            Process staleTar=new ProcessBuilder("tar","-xf",staleOut.toPath().resolve("deployment.erofs").toString(),"-C",staleExtracted.toString()).start();
            if(staleTar.waitFor()!=0)throw new AssertionError("Cannot extract pre-handback image fixture");
            if(Files.exists(staleExtracted.resolve("extra/chrome")))
                throw new AssertionError("pre-handback snapshot unexpectedly contains extra-data payload");

            File imageOut=root.resolve("published-image").toFile();
            // StagedPacks must take its local OSTree snapshot and read its
            // image entries again after mergeExtra has changed this tree.
            List<ObjectPacks.Entry> entries=appImageEntries(deployed.toFile());
            ErofsImage.build(entries,imageOut,fakeMkfs,0);
            Path extracted=root.resolve("published");Files.createDirectories(extracted);
            Process tar=new ProcessBuilder("tar","-xf",imageOut.toPath().resolve("deployment.erofs").toString(),"-C",extracted.toString()).start();
            if(tar.waitFor()!=0)throw new AssertionError("Cannot extract published image fixture");
            if(!Files.readString(extracted.resolve("bin/chrome")).contains("exec /app/extra/chrome"))
                throw new AssertionError("published image omitted /app/bin/chrome");
            if(!Files.readString(extracted.resolve("extra/chrome")).contains("chrome executable payload"))
                throw new AssertionError("published image omitted returned /app/extra payload");
            System.out.println("PASS extra-data handback is merged into the full published app EROFS image");
        } finally {
            try(var paths=Files.walk(root)) {
                paths.sorted(java.util.Comparator.reverseOrder()).forEach(path->{try{Files.deleteIfExists(path);}catch(Exception ignored){}});
            }
        }
    }

    private static List<ObjectPacks.Entry> appImageEntries(File deployment)throws Exception {
        List<ObjectPacks.Entry> result=new java.util.ArrayList<>();
        for(ObjectPacks.Entry source:ObjectPacks.fromTreeWithModes(deployment,"/app/")) {
            String path=source.path.equals("/app")?"/":source.path.substring("/app".length());
            ObjectPacks.Entry entry=new ObjectPacks.Entry(path,source.checksum,source.kind,source.mode);
            entry.target=source.target;entry.xattrs=source.xattrs;entry.uid=source.uid;entry.gid=source.gid;
            entry.mtime=source.mtime;entry.length=source.length;entry.source=source.source;result.add(entry);
        }
        return result;
    }
}
