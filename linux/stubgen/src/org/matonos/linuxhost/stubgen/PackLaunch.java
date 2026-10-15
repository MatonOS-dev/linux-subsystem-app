package org.matonos.linuxhost.stubgen;

import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;

/** Resolves only PackageManager-supplied carriers; no caller-supplied payload paths. */
public final class PackLaunch {
    public static List<String> arguments(File base, String[] names, String[] paths, File cache) throws Exception {
        byte[] index;
        try(ZipFile zip=new ZipFile(base)) {
            ZipEntry entry=zip.getEntry("assets/matonos/objects.index");
            if(entry==null)return Collections.emptyList();
            try(InputStream in=zip.getInputStream(entry)){index=in.readAllBytes();}
        }
        Set<String> needed=packs(index);
        Map<String,String> carriers=new HashMap<>();
        if(names!=null || paths!=null) {
            if(names==null || paths==null || names.length!=paths.length)throw new IOException("Inconsistent package splits");
            for(int i=0;i<names.length;i++) {
                if(names[i].matches("pack_[0-9a-f]{64}")) {
                    String pack="pack-"+names[i].substring(5)+".zip";
                    if(carriers.put(pack,paths[i])!=null)throw new IOException("Duplicate content split");
                }
            }
        }
        if(!carriers.keySet().containsAll(needed))throw new IOException("Missing installed content split");
        String hash=ObjectPacks.hex(MessageDigest.getInstance("SHA-256").digest(index));
        Path directory=new File(cache,"pack-mount-"+hash).toPath();Files.createDirectories(directory);
        // Only the index and tiny carrier-name links are materialized. The loader
        // opens and pins the real APK descriptors before any payload runs.
        for(String pack:needed) {
            Path target=Path.of(carriers.get(pack));
            if(!target.isAbsolute() || !Files.isRegularFile(target))throw new IOException("Invalid installed carrier");
            Path link=directory.resolve(pack),temp=Files.createTempFile(directory,"carrier-",".tmp");
            try {
                Files.delete(temp);Files.createSymbolicLink(temp,target);
                Files.move(temp,link,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
            } finally {Files.deleteIfExists(temp);}
        }
        Path temp=Files.createTempFile(directory,"index-",".tmp"),destination=directory.resolve("objects.index");
        try {Files.write(temp,index);Files.move(temp,destination,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);}
        finally {Files.deleteIfExists(temp);}
        return Arrays.asList("--pack-mount","/app",destination.toString(),directory.toString());
    }
    static Set<String> packs(byte[] index) throws IOException {
        try {
            ByteBuffer b=ByteBuffer.wrap(index).order(ByteOrder.LITTLE_ENDIAN);
            byte[] magic=new byte[8];b.get(magic);
            if(!Arrays.equals(magic,"MTPKIDX1".getBytes(StandardCharsets.US_ASCII)))throw new IOException("Invalid pack index");
            long count=Integer.toUnsignedLong(b.getInt());Set<String> packs=new TreeSet<>();
            for(long i=0;i<count;i++) {
                String path=string(b),checksum=string(b),pack=string(b),kind=string(b);string(b);blob(b);
                long offset=b.getLong(),length=b.getLong();b.getInt();b.getInt();b.getInt();
                if(!path.startsWith("/")||!checksum.matches("[0-9a-f]{64}")||offset<0||length<0||offset>Long.MAX_VALUE-length)
                    throw new IOException("Invalid indexed object");
                if(kind.equals("file")) {
                    if(!pack.matches("pack-[0-9a-f]{64}\\.zip")||offset%4096!=0)throw new IOException("Invalid indexed carrier");
                    packs.add(pack);
                } else if(!(kind.equals("dir")||kind.equals("symlink"))||!pack.isEmpty()||offset!=0||length!=0)
                    throw new IOException("Invalid metadata entry");
            }
            if(b.hasRemaining())throw new IOException("Trailing index bytes");return packs;
        } catch(BufferUnderflowException|IllegalArgumentException e){throw new IOException("Malformed pack index",e);}
    }
    private static byte[] blob(ByteBuffer b) throws IOException {
        int length=b.getInt();if(length<0||length>b.remaining())throw new IOException("Truncated index field");
        byte[] data=new byte[length];b.get(data);return data;
    }
    private static String string(ByteBuffer b)throws IOException {return new String(blob(b),StandardCharsets.UTF_8);}
    private PackLaunch() { }
}
