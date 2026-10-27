package org.matonos.linux.stub;

import org.matonos.linuxhost.stubgen.PlaceholderFiles;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermission;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;

public final class ExtraImageValidatorTest {
    public static void main(String[] args)throws Exception {
        Path temp=Files.createTempDirectory("extra-image-test");
        try {
            rejectsFsck(temp,"#!/bin/sh\necho fsck diagnostic >&2\nexit 7\n","exit status 7","fsck diagnostic");
            rejectsFsck(temp,"#!/bin/sh\nkill -TERM $$\n","exit status 143","possible signal 15 (SIGTERM");
            Path fsck=Files.createTempFile(temp,"fsck-preserve-perms-", ".sh");
            Files.writeString(fsck,"#!/bin/sh\ncase \" $* \" in *\" --preserve-perms \"*) exit 0;; *) exit 13;; esac\n");
            fsck.toFile().setExecutable(true);
            Path emptyExtract=temp.resolve("extract-preserve-perms");
            ExtraImageValidator.checkImage(fsck.toFile(),temp.resolve("test.erofs").toFile(),emptyExtract.toFile(),java.util.Collections.emptyList());
            remove(emptyExtract);Files.delete(fsck);

            Path root=Files.createDirectories(temp.resolve("good/app/bin"));
            Files.writeString(root.resolve("run"),"trusted");mode(root.resolve("run"),0755);
            Path include=Files.createDirectories(temp.resolve("good/app/include"));
            Files.writeString(include.resolve("header.h"),"committed-header");mode(include.resolve("header.h"),0644);
            Path trustedStore=Files.createDirectories(temp.resolve("trusted-store"));
            mode(trustedStore,0700);
            Path inodeStore=Files.createDirectory(trustedStore.resolve(".v3-inodes"));mode(inodeStore,0700);
            Path storeMarker=Files.createFile(trustedStore.resolve(".v3-store-root"));mode(storeMarker,0600);
            Path backing=inodeStore.resolve("i-0123456789abcdef0123456789abcdef");
            Files.writeString(backing,"committed-header");mode(backing,0644);
            Path trustedHeader=trustedStore.resolve("header.h");Files.createFile(trustedHeader);Files.setAttribute(trustedHeader,"unix:mode",0101600);
            long dev=((Number)Files.getAttribute(trustedHeader,"unix:dev")).longValue();
            long ino=((Number)Files.getAttribute(trustedHeader,"unix:ino")).longValue();
            Files.writeString(inodeStore.resolve(String.format(java.util.Locale.ROOT,"p-%016x-%016x",dev,ino)),
                    "i-0123456789abcdef0123456789abcdef\0");
            if(!PlaceholderFiles.isPlaceholder(trustedHeader.toFile()))
                throw new AssertionError("trusted file is an indexed hardlink marker");
            Path extra=Files.createDirectories(temp.resolve("good/app/extra"));Files.writeString(extra.resolve("blob"),"downloaded");mode(extra.resolve("blob"),0644);
            mode(root.getParent(),0755);mode(root,0755);mode(include,0755);mode(extra,0755);
            mode(temp.resolve("good/app"),0755);mode(temp.resolve("good"),0755);
            List<ExtraImageValidator.Entry> commit=Arrays.asList(
                    entry("/files","dir",0755,null,null),entry("/files/bin","dir",0755,null,null),
                    entry("/files/bin/run","file",0755,sha("trusted"),null),
                    entry("/files/include","dir",0755,null,null),
                    new ExtraImageValidator.Entry("/files/include/header.h","file",0644,null,
                            "OSTree-object-id-is-not-the-file-content-digest",trustedHeader.toFile()),
                    entry("/export","dir",0755,null,null),entry("/metadata","file",0644,null,null));
            List<ExtraImageValidator.Entry> expected=ExtraImageValidator.flatpakFiles(commit,"/app");
            ExtraImageValidator.validate(temp.resolve("good").toFile(),expected);
            if(expected.size()!=5||!expected.stream().anyMatch(e->e.path.equals("/app/include/header.h")))
                throw new AssertionError("Flatpak /files subtree was not projected onto /app");
            try {
                ExtraImageValidator.flatpakFiles(Arrays.asList(entry("/metadata","file",0644,null,null)),"/app");
                throw new AssertionError("accepted Flatpak commit without /files");
            } catch(java.io.IOException goodCommitFailure) {
                if(!goodCommitFailure.getMessage().contains("no /files"))throw goodCommitFailure;
            }

            Path wrongMode=temp.resolve("wrong-mode");copy(temp.resolve("good"),wrongMode);mode(wrongMode.resolve("app"),0700);
            rejects(wrongMode,expected,"wrong committed mode", "expected 0755", "actual 0700");

            Path tampered=temp.resolve("tampered");copy(temp.resolve("good"),tampered);Files.writeString(tampered.resolve("app/bin/run"),"modified");
            rejects(tampered,expected,"tampered non-extra file");

            Path tamperedHardlink=temp.resolve("tampered-hardlink");copy(temp.resolve("good"),tamperedHardlink);
            Files.writeString(tamperedHardlink.resolve("app/include/header.h"),"modified-header");
            mode(tamperedHardlink.resolve("app"),0755);mode(tamperedHardlink.resolve("app/bin"),0755);
            mode(tamperedHardlink.resolve("app/include"),0755);
            rejects(tamperedHardlink,expected,"tampered marker-backed file","expected sha256","actual ");

            Path setuid=temp.resolve("setuid");copy(temp.resolve("good"),setuid);mode(setuid.resolve("app/extra/blob"),04755);
            rejects(setuid,expected,"setuid extra-data file");

            Path escape=temp.resolve("escape");copy(temp.resolve("good"),escape);Files.delete(escape.resolve("app/extra/blob"));
            Files.createSymbolicLink(escape.resolve("app/extra/blob"),temp.resolve("outside"));
            rejects(escape,expected,"extra-data symlink escape");
            System.out.println("PASS EROFS extra-data validation");
        } finally { remove(temp); }
    }
    private static ExtraImageValidator.Entry entry(String path,String kind,int mode,String hash,String target){return new ExtraImageValidator.Entry(path,kind,mode,target,hash,null);}
    private static void rejectsFsck(Path temp,String script,String... expected)throws Exception {
        Path fsck=Files.createTempFile(temp,"fsck-", ".sh");Files.writeString(fsck,script);fsck.toFile().setExecutable(true);
        Path extracted=temp.resolve("extract-"+Files.size(fsck)+"-"+System.nanoTime());
        try {
            ExtraImageValidator.checkImage(fsck.toFile(),temp.resolve("test.erofs").toFile(),extracted.toFile(),java.util.Collections.emptyList());
            throw new AssertionError("accepted failing fsck process");
        } catch(java.io.IOException failure) {
            for(String fragment:expected)if(!failure.getMessage().contains(fragment))
                throw new AssertionError("fsck failure omitted '"+fragment+"': "+failure.getMessage());
        } finally {remove(extracted);Files.deleteIfExists(fsck);}
    }
    private static void rejects(Path root,List<ExtraImageValidator.Entry> expected,String name,String... diagnostic)throws Exception {try{ExtraImageValidator.validate(root.toFile(),expected);throw new AssertionError("accepted "+name);}catch(java.io.IOException good){for(String fragment:diagnostic)if(!good.getMessage().contains(fragment))throw new AssertionError(name+" omitted '"+fragment+"': "+good.getMessage());}}
    private static String sha(String text)throws Exception{byte[] d=MessageDigest.getInstance("SHA-256").digest(text.getBytes(java.nio.charset.StandardCharsets.UTF_8));StringBuilder s=new StringBuilder();for(byte b:d)s.append(String.format("%02x",b&255));return s.toString();}
    private static void mode(Path path,int value)throws Exception{HashSet<PosixFilePermission> p=new HashSet<>();if((value&0400)!=0)p.add(PosixFilePermission.OWNER_READ);if((value&0200)!=0)p.add(PosixFilePermission.OWNER_WRITE);if((value&0100)!=0)p.add(PosixFilePermission.OWNER_EXECUTE);if((value&0040)!=0)p.add(PosixFilePermission.GROUP_READ);if((value&0020)!=0)p.add(PosixFilePermission.GROUP_WRITE);if((value&0010)!=0)p.add(PosixFilePermission.GROUP_EXECUTE);if((value&0004)!=0)p.add(PosixFilePermission.OTHERS_READ);if((value&0002)!=0)p.add(PosixFilePermission.OTHERS_WRITE);if((value&0001)!=0)p.add(PosixFilePermission.OTHERS_EXECUTE);Files.setPosixFilePermissions(path,p);}
    private static void copy(Path a,Path b)throws Exception{try(java.util.stream.Stream<Path>s=Files.walk(a)){for(Path p:(Iterable<Path>)s::iterator){Path q=b.resolve(a.relativize(p));if(Files.isDirectory(p))Files.createDirectories(q);else if(Files.isSymbolicLink(p))Files.createSymbolicLink(q,Files.readSymbolicLink(p));else Files.copy(p,q);}}}
    private static void remove(Path p)throws Exception{if(!Files.exists(p,java.nio.file.LinkOption.NOFOLLOW_LINKS))return;try(java.util.stream.Stream<Path>s=Files.walk(p)){for(Path x:(Iterable<Path>)s.sorted(java.util.Comparator.reverseOrder())::iterator)Files.deleteIfExists(x);}}
}
