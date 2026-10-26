package org.matonos.linux.stub;

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
            Path root=Files.createDirectories(temp.resolve("good/app/bin"));
            Files.writeString(root.resolve("run"),"trusted");mode(root.resolve("run"),0755);
            Path extra=Files.createDirectories(temp.resolve("good/app/extra"));Files.writeString(extra.resolve("blob"),"downloaded");mode(extra.resolve("blob"),0644);
            mode(root.getParent(),0755);mode(root,0755);mode(extra,0755);mode(temp.resolve("good/app"),0755);mode(temp.resolve("good"),0755);
            List<ExtraImageValidator.Entry> expected=Arrays.asList(
                    entry("/app","dir",0755,null,null),entry("/app/bin","dir",0755,null,null),
                    entry("/app/bin/run","file",0755,sha("trusted"),null));
            ExtraImageValidator.validate(temp.resolve("good").toFile(),expected);

            Path tampered=temp.resolve("tampered");copy(temp.resolve("good"),tampered);Files.writeString(tampered.resolve("app/bin/run"),"modified");
            rejects(tampered,expected,"tampered non-extra file");

            Path setuid=temp.resolve("setuid");copy(temp.resolve("good"),setuid);mode(setuid.resolve("app/extra/blob"),04755);
            rejects(setuid,expected,"setuid extra-data file");

            Path escape=temp.resolve("escape");copy(temp.resolve("good"),escape);Files.delete(escape.resolve("app/extra/blob"));
            Files.createSymbolicLink(escape.resolve("app/extra/blob"),temp.resolve("outside"));
            rejects(escape,expected,"extra-data symlink escape");
            System.out.println("PASS EROFS extra-data validation");
        } finally { remove(temp); }
    }
    private static ExtraImageValidator.Entry entry(String path,String kind,int mode,String hash,String target){return new ExtraImageValidator.Entry(path,kind,mode,target,hash,null);}
    private static void rejects(Path root,List<ExtraImageValidator.Entry> expected,String name)throws Exception {try{ExtraImageValidator.validate(root.toFile(),expected);throw new AssertionError("accepted "+name);}catch(java.io.IOException good){}}
    private static String sha(String text)throws Exception{byte[] d=MessageDigest.getInstance("SHA-256").digest(text.getBytes(java.nio.charset.StandardCharsets.UTF_8));StringBuilder s=new StringBuilder();for(byte b:d)s.append(String.format("%02x",b&255));return s.toString();}
    private static void mode(Path path,int value)throws Exception{HashSet<PosixFilePermission> p=new HashSet<>();if((value&0400)!=0)p.add(PosixFilePermission.OWNER_READ);if((value&0200)!=0)p.add(PosixFilePermission.OWNER_WRITE);if((value&0100)!=0)p.add(PosixFilePermission.OWNER_EXECUTE);if((value&0040)!=0)p.add(PosixFilePermission.GROUP_READ);if((value&0020)!=0)p.add(PosixFilePermission.GROUP_WRITE);if((value&0010)!=0)p.add(PosixFilePermission.GROUP_EXECUTE);if((value&0004)!=0)p.add(PosixFilePermission.OTHERS_READ);if((value&0002)!=0)p.add(PosixFilePermission.OTHERS_WRITE);if((value&0001)!=0)p.add(PosixFilePermission.OTHERS_EXECUTE);Files.setPosixFilePermissions(path,p);}
    private static void copy(Path a,Path b)throws Exception{try(java.util.stream.Stream<Path>s=Files.walk(a)){for(Path p:(Iterable<Path>)s::iterator){Path q=b.resolve(a.relativize(p));if(Files.isDirectory(p))Files.createDirectories(q);else if(Files.isSymbolicLink(p))Files.createSymbolicLink(q,Files.readSymbolicLink(p));else Files.copy(p,q);}}}
    private static void remove(Path p)throws Exception{if(!Files.exists(p,java.nio.file.LinkOption.NOFOLLOW_LINKS))return;try(java.util.stream.Stream<Path>s=Files.walk(p)){for(Path x:(Iterable<Path>)s.sorted(java.util.Comparator.reverseOrder())::iterator)Files.deleteIfExists(x);}}
}
