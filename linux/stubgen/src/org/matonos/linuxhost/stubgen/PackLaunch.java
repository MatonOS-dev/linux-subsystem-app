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
        byte[] index=null;String indexPath=null;
        if(names==null || paths==null || names.length!=paths.length)
            throw new IOException("Missing or inconsistent package splits");
        int selected=newestIndex(Arrays.asList(names));
        if(selected>=0)indexPath=paths[selected];
        if(indexPath==null)throw new IOException("Missing index split");
        try(ZipFile zip=new ZipFile(indexPath)) {
            ZipEntry entry=zip.getEntry("assets/matonos/objects.index");
            if(entry==null)throw new IOException("Index split has no index");
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
    /** All index names must parse; duplicate revisions fail even below the newest. */
    public static int newestIndex(List<String> names) throws IOException {
        int selected=-1,highest=0;Set<Integer> revisions=new HashSet<>();
        for(int i=0;i<names.size();i++) {
            String name=names.get(i);
            if(name==null)throw new IOException("Null split name");
            if(!name.startsWith("index_"))continue;
            if(!name.matches("index_r[1-9][0-9]*_[0-9a-f]{64}"))throw new IOException("Invalid index name: "+name);
            int revision;
            try {revision=Integer.parseInt(name.substring(7,name.indexOf('_',7)));}
            catch(NumberFormatException e){throw new IOException("Index revision overflow",e);}
            if(!revisions.add(revision))throw new IOException("Index revision tie");
            if(revision>highest){highest=revision;selected=i;}
        }
        return selected;
    }
    public static Set<String> referencedSplits(File indexApk) throws IOException {
        try(ZipFile zip=new ZipFile(indexApk)) {
            ZipEntry entry=zip.getEntry("assets/matonos/objects.index");
            if(entry==null)throw new IOException("Missing index");
            Set<String> result=new TreeSet<>();
            try(InputStream in=zip.getInputStream(entry)) {
                for(String pack:packs(in.readAllBytes()))result.add("pack_"+pack.substring(5,69));
            }
            return result;
        }
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
