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
    /** Associate a virtual pack with its disposable Flatpak deployment files directory.
     * The loader consumes this immediately after the corresponding pack mount. */
    public static List<String> backingArguments(String mountPoint,File files) throws IOException {
        if(mountPoint==null||!mountPoint.matches("/(?:[A-Za-z0-9_.+-]+/?)*")
                ||files==null||!files.isAbsolute())throw new IOException("Invalid pack backing");
        return Arrays.asList("--erofs-backing",mountPoint,files.toPath().normalize().toString());
    }
    public static List<String> arguments(File base, String[] names, String[] paths, File cache) throws Exception {
        return arguments(base,names,paths,cache,"/app");
    }
    public static List<String> arguments(File base, String[] names, String[] paths, File cache,String mountPoint) throws Exception {
        ArrayList<String> out=new ArrayList<>();
        String indexPath=indexPath(names,paths);
        out.addAll(mount(readIndex(indexPath),names,paths,cache,mountPoint,null));
        return out;
    }
    public static List<String> originalArguments(String[] names,String[] paths,File cache) throws Exception {
        try(ZipFile zip=new ZipFile(indexPath(names,paths))) {
            ZipEntry entry=zip.getEntry("assets/matonos/original.index");
            if(entry==null)throw new IOException("Installed index lacks original file identities");
            try(InputStream in=zip.getInputStream(entry)) {
                return mount(in.readAllBytes(),names,paths,cache,"/app",null);
            }
        }
    }
    /** Base pack mounts at {@code mountPoint} plus, when the package has an
     * {@code extra_} split, a second mount of its self-contained output at
     * {@code extraMountPoint}. The extra split carries its own index. */
    public static List<String> argumentsWithExtra(File base, String[] names, String[] paths, File cache,
            String mountPoint,String extraMountPoint) throws Exception {
        ArrayList<String> out=new ArrayList<>(arguments(base,names,paths,cache,mountPoint));
        if(mountPoint==null || !mountPoint.matches("/(?:[A-Za-z0-9_.+-]+/?)*")
                || extraMountPoint==null || !extraMountPoint.matches("/(?:[A-Za-z0-9_.+-]+/?)*")
                || !extraMountPoint.startsWith(mountPoint+"/"))
            throw new IOException("Invalid extra pack mount point");
        String extraPath=null;
        if(names!=null&&paths!=null&&names.length==paths.length)
            for(int i=0;i<names.length;i++)if(names[i].matches("extra_[0-9a-f]{64}")) {
                if(extraPath!=null)throw new IOException("Multiple extra splits");
                extraPath=paths[i];
            }
        if(extraPath!=null)
            out.addAll(mount(readIndex(extraPath),null,null,cache,extraMountPoint,extraPath));
        return out;
    }
    /** Mount a subsystem-owned pack set stored as APK assets, such as GL extensions. */
    public static List<String> argumentsFromArchive(File archive,String assetPrefix,File cache,
            String mountPoint) throws Exception {
        if(assetPrefix==null||!assetPrefix.matches("[A-Za-z0-9_.+-]+(?:/[A-Za-z0-9_.+-]+)*"))
            throw new IOException("Invalid packaged extension path");
        byte[] index;
        Map<String,String> carriers=new HashMap<>();
        try(ZipFile zip=new ZipFile(archive)) {
            ZipEntry entry=zip.getEntry(assetPrefix+"/objects.index");
            if(entry==null)throw new IOException("Missing packaged extension index");
            try(InputStream input=zip.getInputStream(entry)){index=input.readAllBytes();}
            Path directory=new File(cache,"extension-carriers").toPath();Files.createDirectories(directory);
            for(String pack:packs(index)) {
                ZipEntry carrier=zip.getEntry(assetPrefix+"/"+pack);
                if(carrier==null)throw new IOException("Missing packaged extension carrier");
                Path destination=directory.resolve(pack).normalize();
                if(!destination.getParent().equals(directory))throw new IOException("Invalid extension carrier name");
                try(InputStream input=zip.getInputStream(carrier);OutputStream output=Files.newOutputStream(destination)) {
                    input.transferTo(output);
                }
                carriers.put(pack,destination.toString());
            }
        }
        return mount(index,carriers,cache,mountPoint);
    }
    private static String indexPath(String[] names,String[] paths) throws IOException {
        if(names==null || paths==null || names.length!=paths.length)
            throw new IOException("Missing or inconsistent package splits");
        String indexPath=null;
        for(int i=0;i<names.length;i++)if(names[i].matches("index_[0-9a-f]{64}")) {
            if(indexPath!=null)throw new IOException("Multiple index splits");
            indexPath=paths[i];
        }
        if(indexPath==null)throw new IOException("Missing index split");
        return indexPath;
    }
    /** The embedded index of an {@code index_} or self-contained {@code extra_} split. */
    private static byte[] readIndex(String apkPath) throws IOException {
        try(ZipFile zip=new ZipFile(apkPath)) {
            ZipEntry entry=zip.getEntry("assets/matonos/objects.index");
            if(entry==null)throw new IOException("Index split has no index");
            try(InputStream in=zip.getInputStream(entry)){return in.readAllBytes();}
        }
    }
    /** Materializes one pack mount: the index, plus one symlink per needed
     * carrier name. Only the index and tiny links touch the cache; the loader
     * opens and pins the real APK descriptors before any payload runs. */
    private static List<String> mount(byte[] index,String[] names,String[] paths,File cache,
            String mountPoint,String onlyCarrier) throws Exception {
        if(mountPoint==null || !mountPoint.matches("/(?:[A-Za-z0-9_.+-]+/?)*"))throw new IOException("Invalid pack mount point");
        Set<String> needed=packs(index);
        Map<String,String> carriers=new HashMap<>();
        if(onlyCarrier!=null) {
            for(String pack:needed)carriers.put(pack,onlyCarrier);
        } else {
            for(int i=0;i<names.length;i++) {
                if(names[i].matches("pack_[0-9a-f]{64}")) {
                    String pack="pack-"+names[i].substring(5)+".zip";
                    if(carriers.put(pack,paths[i])!=null)throw new IOException("Duplicate content split");
                }
            }
        }
        if(!carriers.keySet().containsAll(needed))throw new IOException("Missing installed content split");
        return mount(index,carriers,cache,mountPoint);
    }
    private static List<String> mount(byte[] index,Map<String,String> carriers,File cache,String mountPoint) throws Exception {
        Set<String> needed=packs(index);
        if(!carriers.keySet().containsAll(needed))throw new IOException("Missing installed content split");
        String hash=ObjectPacks.hex(MessageDigest.getInstance("SHA-256").digest(index));
        Path directory=new File(cache,"pack-mount-"+hash).toPath();Files.createDirectories(directory);
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
        return Arrays.asList("--pack-mount",mountPoint,destination.toString(),directory.toString());
    }
    static Set<String> packs(byte[] index) throws IOException {
        try {
            ByteBuffer b=ByteBuffer.wrap(index).order(ByteOrder.LITTLE_ENDIAN);
            if(index==null || index.length<12)throw new IOException("Truncated pack index");
            byte[] magic=new byte[8];b.get(magic);
            if(!Arrays.equals(magic,"MTPKIDX1".getBytes(StandardCharsets.US_ASCII)))throw new IOException("Invalid pack index");
            long count=Integer.toUnsignedLong(b.getInt());
            if(count>b.remaining()/4L)throw new IOException("Invalid pack index entry count");
            Set<String> packs=new TreeSet<>();
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
