package org.matonos.linux.stub;

import android.os.Binder;
import android.os.IBinder;
import android.util.Log;
import org.matonos.erofs.IErofsMounter;
import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;

/** Mounts every signed EROFS layer for one installed package before proxy start. */
final class ErofsLaunch {
    private static final String TAG="MatonErofsLaunch";
    private static final IBinder TOKEN=new Binder();
    private ErofsLaunch(){}

    static List<String> arguments(File base,String[] names,String[] paths,File cache,String target) throws Exception {
        if(target==null||!target.matches("/(?:[A-Za-z0-9_.+-]+/?)*"))throw new IOException("Invalid EROFS deployment mount point");
        if(names==null||paths==null||names.length!=paths.length)throw new IOException("Missing package split paths for EROFS layers");
        byte[] index;
        try(ZipFile zip=new ZipFile(base)) {
            ZipEntry e=zip.getEntry("assets/matonos/layers.index");
            if(e==null)throw new IOException("Package lacks signed layers.index");
            try(InputStream in=zip.getInputStream(e)){index=in.readAllBytes();}
        }
        ByteBuffer b=ByteBuffer.wrap(index).order(ByteOrder.LITTLE_ENDIAN);
        if(b.remaining()<12||!Arrays.equals(read(b,8),"MTPKIDX3".getBytes(StandardCharsets.US_ASCII)))throw new IOException("Invalid MTPKIDX3 layer index");
        long count=Integer.toUnsignedLong(b.getInt());
        Map<String,String> splitPaths=new HashMap<>();for(int i=0;i<names.length;i++)splitPaths.put(names[i],paths[i]);
        Map<String,String> mounted=new LinkedHashMap<>();
        IErofsMounter service=mounter();
        for(int i=0;i<count;i++) {
            String split=string(b),entry=string(b);long size=b.getLong();byte[] hash=read(b,32);
            String apk=splitPaths.get(split);
            if(apk==null||!split.matches("pack_[0-9a-f]{64}")||!split.substring(5).equals(hex(hash))||!entry.equals("assets/matonos/pack.erofs")||size<=0||hash.length!=32)
                throw new IOException("Invalid EROFS layer descriptor: "+split);
            String path=service.mountSplit(apk,entry,TOKEN);
            if(path==null||!path.startsWith("/mnt/matonos/"))throw new IOException("EROFS daemon returned an invalid mount path for "+split);
            if(mounted.put(split,path)!=null)throw new IOException("Duplicate EROFS layer split: "+split);
        }
        if(b.remaining()<4)throw new IOException("Truncated layer records");
        long records=Integer.toUnsignedLong(b.getInt());
        for(long i=0;i<records;i++) {
            string(b);string(b);string(b);string(b);blob(b);if(b.remaining()<36)throw new IOException("Truncated layer metadata");
            long layer=Integer.toUnsignedLong(b.getInt());b.position(b.position()+28);int reserved=b.getInt(); // size, mtime, uid, gid, mode
            if(reserved!=0||(layer!=0xffffffffL&&layer>=count))throw new IOException("Invalid layer reference");
        }
        if(b.hasRemaining())throw new IOException("Trailing layer index data");
        String indexHash=org.matonos.linuxhost.stubgen.ObjectPacks.hex(MessageDigest.getInstance("SHA-256").digest(index));
        File directory=new File(cache,"erofs-"+indexHash+"-"+target.replace('/','_'));
        if(!directory.isDirectory()&&!directory.mkdirs())throw new IOException("Cannot create EROFS layer table");
        for(Map.Entry<String,String> e:mounted.entrySet()) {
            Path link=new File(directory,e.getKey()).toPath();Files.deleteIfExists(link);Files.createSymbolicLink(link,Path.of(e.getValue()));
        }
        File indexFile=new File(directory,"layers.index");
        try(FileOutputStream out=new FileOutputStream(indexFile)){out.write(index);out.getFD().sync();}
        Log.i(TAG,"Mounted "+mounted.size()+" EROFS layers at "+target);
        return Arrays.asList("--erofs-mount",target,indexFile.getAbsolutePath(),directory.getAbsolutePath());
    }

    private static IErofsMounter mounter() throws Exception {
        IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"matonos.erofs");
        IErofsMounter result=IErofsMounter.Stub.asInterface(binder);if(result==null)throw new IOException("matonos-erofsd is unavailable");return result;
    }
    private static byte[] read(ByteBuffer b,int n)throws IOException{if(n<0||b.remaining()<n)throw new IOException("Truncated layers.index");byte[] v=new byte[n];b.get(v);return v;}
    private static byte[] blob(ByteBuffer b)throws IOException{if(b.remaining()<4)throw new IOException("Truncated layers.index");long n=Integer.toUnsignedLong(b.getInt());if(n>b.remaining())throw new IOException("Truncated layers.index field");return read(b,(int)n);}
    private static String string(ByteBuffer b)throws IOException{return new String(blob(b),StandardCharsets.UTF_8);}
    private static String hex(byte[] bytes){StringBuilder s=new StringBuilder(bytes.length*2);for(byte value:bytes)s.append(String.format(Locale.ROOT,"%02x",value&255));return s.toString();}
}
