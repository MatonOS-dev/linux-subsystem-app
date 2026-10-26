package org.matonos.linuxhost.stubgen;

import java.io.File;
import java.io.IOException;
import java.util.Arrays;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Metadata helpers for the installed image and disposable object-index format. */
public final class PackLaunch {
    /** Associate a virtual pack with its disposable Flatpak deployment files directory. */
    public static List<String> backingArguments(String mountPoint,File files) throws IOException {
        if(mountPoint==null||!mountPoint.matches("/(?:[A-Za-z0-9_.+-]+/?)*")
                ||files==null||!files.isAbsolute())throw new IOException("Invalid pack backing");
        return Arrays.asList("--erofs-backing",mountPoint,files.toPath().normalize().toString());
    }

    /** Read non-payload metadata from the installed index split; content files live in EROFS. */
    public static byte[] installedIndexAsset(String[] names,String[] paths,String assetName) throws IOException {
        if(assetName==null||!assetName.matches("(?:original\\.index|objects\\.index)"))
            throw new IOException("Invalid installed index asset");
        try(ZipFile zip=new ZipFile(indexPath(names,paths))) {
            ZipEntry entry=zip.getEntry("assets/matonos/"+assetName);
            if(entry==null)throw new IOException("Installed index lacks "+assetName);
            try(java.io.InputStream input=zip.getInputStream(entry)){return input.readAllBytes();}
        }
    }

    private static String indexPath(String[] names,String[] paths) throws IOException {
        if(names==null||paths==null||names.length!=paths.length)throw new IOException("Missing or inconsistent package splits");
        String path=null;
        for(int i=0;i<names.length;i++)if(names[i].matches("index_[0-9a-f]{64}")) {
            if(path!=null)throw new IOException("Multiple index splits");path=paths[i];
        }
        if(path==null)throw new IOException("Missing index split");return path;
    }

    /** Validate an object index while retaining its internal temporary-pack schema. */
    static Set<String> packs(byte[] index) throws IOException {
        try {
            ByteBuffer b=ByteBuffer.wrap(index).order(ByteOrder.LITTLE_ENDIAN);
            if(index==null||index.length<12)throw new IOException("Truncated pack index");
            byte[] magic=new byte[8];b.get(magic);
            if(!Arrays.equals(magic,"MTPKIDX1".getBytes(StandardCharsets.US_ASCII)))throw new IOException("Invalid pack index");
            long count=Integer.toUnsignedLong(b.getInt());
            if(count>b.remaining()/4L)throw new IOException("Invalid pack index entry count");
            Set<String> packs=new TreeSet<>();Set<String> paths=new HashSet<>();
            for(long i=0;i<count;i++) {
                String path=string(b),checksum=string(b),pack=string(b),kind=string(b);string(b);blob(b);
                long offset=b.getLong(),length=b.getLong();b.getInt();b.getInt();b.getInt();
                if(!path.startsWith("/")||!paths.add(path)||!checksum.matches("[0-9a-f]{64}")||offset<0||length<0||offset>Long.MAX_VALUE-length)
                    throw new IOException("Invalid indexed object");
                if(kind.equals("file")) {
                    if(!pack.matches("pack-[0-9a-f]{64}\\.zip")||offset%4096!=0)throw new IOException("Invalid indexed carrier");
                    packs.add(pack);
                } else if(!(kind.equals("dir")||kind.equals("symlink"))||!pack.isEmpty()||offset!=0||length!=0)
                    throw new IOException("Invalid metadata entry");
            }
            if(b.hasRemaining())throw new IOException("Trailing index bytes");return packs;
        } catch(java.nio.BufferUnderflowException|IllegalArgumentException e){throw new IOException("Malformed pack index",e);}
    }
    private static byte[] blob(ByteBuffer b) throws IOException {
        int length=b.getInt();if(length<0||length>b.remaining())throw new IOException("Truncated index field");
        byte[] data=new byte[length];b.get(data);return data;
    }
    private static String string(ByteBuffer b)throws IOException {return new String(blob(b),StandardCharsets.UTF_8);}
    private PackLaunch() { }
}
