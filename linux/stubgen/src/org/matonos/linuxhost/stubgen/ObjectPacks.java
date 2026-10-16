package org.matonos.linuxhost.stubgen;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.CRC32;

/** Deterministic format-v1 writer. No Android dependencies; payloads are streamed. */
public final class ObjectPacks {
    public static final long DEFAULT_PACK_SIZE = 64L * 1024 * 1024;
    public static final class Entry {
        public String path, checksum, pack = "", kind, target = "";
        public byte[] xattrs = new byte[0];
        public long offset, length;
        public int uid, gid, mode;
        public File source;
        public Entry(String path, String checksum, String kind, int mode) {
            this.path=path; this.checksum=checksum; this.kind=kind; this.mode=mode;
        }
    }
    private static final Comparator<String> UTF8 = (a,b) -> {
        byte[] x=a.getBytes(StandardCharsets.UTF_8), y=b.getBytes(StandardCharsets.UTF_8);
        for(int i=0;i<Math.min(x.length,y.length);i++) {
            int d=(x[i]&255)-(y[i]&255); if(d!=0)return d;
        }
        return x.length-y.length;
    };
    private static void le(OutputStream out,long n,int width) throws IOException {
        for(int i=0;i<width;i++){out.write((int)n&255);n>>>=8;}
    }
    private static void bytes(OutputStream out,byte[] value) throws IOException {
        le(out,value.length,4);out.write(value);
    }
    private static void string(OutputStream out,String value) throws IOException {
        bytes(out,value.getBytes(StandardCharsets.UTF_8));
    }
    public static void writeIndex(File file,List<Entry> entries) throws IOException {
        List<Entry> sorted=new ArrayList<>(entries);sorted.sort((a,b)->UTF8.compare(a.path,b.path));
        try(OutputStream out=new BufferedOutputStream(new FileOutputStream(file))) {
            out.write("MTPKIDX1".getBytes(StandardCharsets.US_ASCII));le(out,sorted.size(),4);
            String previous=null;
            for(Entry e:sorted) {
                if(!e.path.startsWith("/") || e.path.equals(previous) || !e.checksum.matches("[0-9a-f]{64}"))
                    throw new IOException("Invalid pack entry");
                previous=e.path;
                for(String s:new String[]{e.path,e.checksum,e.pack,e.kind,e.target})string(out,s);
                bytes(out,e.xattrs);le(out,e.offset,8);le(out,e.length,8);
                le(out,e.uid,4);le(out,e.gid,4);le(out,e.mode,4);
            }
        }
    }
    /** Reads the same bounded format validated by the launcher. Payloads stay in carriers. */
    public static List<Entry> readIndex(File file) throws IOException {
        byte[] bytes=Files.readAllBytes(file.toPath());PackLaunch.packs(bytes);
        java.nio.ByteBuffer b=java.nio.ByteBuffer.wrap(bytes).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        b.position(8);int count=b.getInt();List<Entry> result=new ArrayList<>();
        for(int i=0;i<count;i++) {
            String path=field(b),checksum=field(b),pack=field(b),kind=field(b),target=field(b);
            Entry e=new Entry(path,checksum,kind,0);e.pack=pack;e.target=target;
            e.xattrs=new byte[b.getInt()];b.get(e.xattrs);e.offset=b.getLong();e.length=b.getLong();
            e.uid=b.getInt();e.gid=b.getInt();e.mode=b.getInt();result.add(e);
        }
        return result;
    }
    private static String field(java.nio.ByteBuffer b) {
        byte[] bytes=new byte[b.getInt()];b.get(bytes);return new String(bytes,StandardCharsets.UTF_8);
    }
    public static void writeMetadata(File file,Map<String,byte[]> objects) throws Exception {
        try(OutputStream out=new BufferedOutputStream(new FileOutputStream(file))) {
            out.write("MTPKOBJ1".getBytes(StandardCharsets.US_ASCII));le(out,objects.size(),4);
            for(String checksum:new TreeSet<>(objects.keySet())) {
                if(!checksum.matches("[0-9a-f]{64}"))throw new IOException("Invalid metadata checksum");
                byte[] raw=objects.get(checksum);
                if(!hex(MessageDigest.getInstance("SHA-256").digest(raw)).equals(checksum))
                    throw new IOException("Metadata checksum mismatch");
                for(int i=0;i<64;i+=2)out.write(Integer.parseInt(checksum.substring(i,i+2),16));
                le(out,raw.length,8);out.write(raw);
            }
        }
    }
    public static String hex(byte[] bytes) {
        StringBuilder s=new StringBuilder();for(byte b:bytes)s.append(String.format(Locale.ROOT,"%02x",b&255));return s.toString();
    }
    private static String digest(File file) throws Exception {
        MessageDigest d=MessageDigest.getInstance("SHA-256");
        try(InputStream in=new BufferedInputStream(new FileInputStream(file))) {
            byte[] b=new byte[65536];int n;while((n=in.read(b))!=-1)d.update(b,0,n);
        }
        return hex(d.digest());
    }
    private static Map<String,Entry> unique(List<Entry> entries) {
        Map<String,Entry> out=new LinkedHashMap<>();
        for(Entry e:entries)if(e.kind.equals("file"))out.putIfAbsent(e.checksum,e);
        return out;
    }
    private static void propagate(List<Entry> entries,Collection<Entry> packed) {
        Map<String,Entry> by=new HashMap<>();for(Entry e:packed)by.put(e.checksum,e);
        for(Entry e:entries){Entry p=by.get(e.checksum);if(e.kind.equals("file")&&p!=null){e.pack=p.pack;e.offset=p.offset;e.length=p.length;}}
    }
    /** Byte-for-byte Python zipfile output, including Unix attributes and alignment extras.
     * ZIP32 limits fail explicitly; no silently truncated offsets. */
    private static void zip(File file,Collection<Entry> entries) throws Exception {
        List<Entry> sorted=new ArrayList<>(entries);sorted.sort(Comparator.comparing(e->e.checksum));
        if(sorted.size()>65535)throw new IOException("ZIP64 pack required");
        ByteArrayOutputStream central=new ByteArrayOutputStream();
        try(RandomAccessFile out=new RandomAccessFile(file,"rw")) {
            out.setLength(0);
            for(Entry e:sorted) {
                if(e.source==null || !e.source.isFile() || !e.checksum.matches("[0-9a-f]{64}"))throw new IOException("Missing object payload");
                long length=e.source.length(), start=out.getFilePointer();
                if(length>=0xffffffffL || start>=0xffffffffL)throw new IOException("ZIP64 pack required");
                CRC32 crc=new CRC32();byte[] buffer=new byte[65536];int n;
                try(InputStream in=new BufferedInputStream(new FileInputStream(e.source))){while((n=in.read(buffer))!=-1)crc.update(buffer,0,n);}
                byte[] name=e.checksum.getBytes(StandardCharsets.US_ASCII);
                int pad=(int)(-(start+30+name.length+4)&4095);
                ByteArrayOutputStream extra=new ByteArrayOutputStream();le(extra,0x4d54,2);le(extra,pad,2);extra.write(new byte[pad]);
                ByteArrayOutputStream local=new ByteArrayOutputStream();
                le(local,0x04034b50,4);le(local,20,2);le(local,0,2);le(local,0,2);le(local,0,2);le(local,33,2);
                le(local,crc.getValue(),4);le(local,length,4);le(local,length,4);le(local,name.length,2);le(local,extra.size(),2);
                local.write(name);extra.writeTo(local);out.write(local.toByteArray());
                e.offset=out.getFilePointer();e.length=length;
                CRC32 written=new CRC32();
                try(InputStream in=new BufferedInputStream(new FileInputStream(e.source))){while((n=in.read(buffer))!=-1){out.write(buffer,0,n);written.update(buffer,0,n);}}
                if(written.getValue()!=crc.getValue() || out.getFilePointer()!=e.offset+length)throw new IOException("Payload changed while packing");
                le(central,0x02014b50,4);le(central,0x0314,2);le(central,20,2);le(central,0,2);le(central,0,2);le(central,0,2);le(central,33,2);
                le(central,crc.getValue(),4);le(central,length,4);le(central,length,4);le(central,name.length,2);le(central,extra.size(),2);
                le(central,0,2);le(central,0,2);le(central,0,2);le(central,(long)(e.mode&65535)<<16,4);le(central,start,4);
                central.write(name);extra.writeTo(central);
            }
            long start=out.getFilePointer();
            if(start+central.size()>=0xffffffffL)throw new IOException("ZIP64 pack required");
            out.write(central.toByteArray());ByteArrayOutputStream end=new ByteArrayOutputStream();
            le(end,0x06054b50,4);le(end,0,2);le(end,0,2);le(end,sorted.size(),2);le(end,sorted.size(),2);
            le(end,central.size(),4);le(end,start,4);le(end,0,2);out.write(end.toByteArray());
        }
    }
    private static void emit(File directory,List<Entry> all,Collection<Entry> batch) throws Exception {
        File temp=File.createTempFile("pack-",".zip",directory);
        try {
            zip(temp,batch);String name="pack-"+digest(temp)+".zip";
            Files.move(temp.toPath(),new File(directory,name).toPath(),java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            for(Entry e:batch)e.pack=name;propagate(all,batch);
        } finally {temp.delete();}
    }
    public static void initial(List<Entry> entries,File directory,long targetSize) throws Exception {
        if(targetSize<1)throw new IllegalArgumentException("Invalid pack target");
        Files.createDirectories(directory.toPath());
        Map<String,List<Entry>> groups=new TreeMap<>(UTF8);
        for(Entry e:unique(entries).values()) {
            String parent=e.path.substring(0,e.path.lastIndexOf('/'));if(parent.isEmpty())parent="/";
            groups.computeIfAbsent(parent,k->new ArrayList<>()).add(e);
        }
        List<Entry> batch=new ArrayList<>();long size=0;
        for(List<Entry> group:groups.values()) {
            group.sort((a,b)->UTF8.compare(a.path,b.path));
            for(Entry e:group) {
                long n=e.source.length();if(!batch.isEmpty()&&n>targetSize-size){emit(directory,entries,batch);batch.clear();size=0;}
                batch.add(e);size+=n;
            }
        }
        if(!batch.isEmpty())emit(directory,entries,batch);
    }
    /** Reuses old carriers, repacking a touched pack at the default 0.5 dead-byte ratio.
     * Fresh entries must retain sources even for unchanged objects. */
    public static void update(List<Entry> fresh,List<Entry> old,File oldDirectory,File output,double threshold) throws Exception {
        if(!Double.isFinite(threshold)||threshold<0||threshold>1)throw new IllegalArgumentException("Invalid dead ratio");
        Files.createDirectories(output.toPath());Map<String,Entry> prior=unique(old), live=unique(fresh);
        List<Entry> added=new ArrayList<>();
        for(Entry e:live.values()) {
            Entry p=prior.get(e.checksum);
            if(p==null)added.add(e);else {e.pack=p.pack;e.offset=p.offset;e.length=p.length;}
        }
        propagate(fresh,live.values());if(!added.isEmpty())emit(output,fresh,added);
        Map<String,List<Entry>> packs=new TreeMap<>();
        for(Entry e:prior.values())packs.computeIfAbsent(e.pack,k->new ArrayList<>()).add(e);
        for(Map.Entry<String,List<Entry>> p:packs.entrySet()) {
            long total=0,dead=0;List<Entry> keep=new ArrayList<>();
            for(Entry e:p.getValue()){total=Math.addExact(total,e.length);if(live.containsKey(e.checksum))keep.add(live.get(e.checksum));else dead=Math.addExact(dead,e.length);}
            if(keep.isEmpty())continue;
            if(total>0&&dead>0&&(double)dead/total>=threshold)emit(output,fresh,keep);
            else Files.copy(new File(oldDirectory,p.getKey()).toPath(),new File(output,p.getKey()).toPath(),java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        }
    }
    /** Appends a manifest without moving any indexed payload byte. ZIP32 only. */
    public static void carrier(File pack, File output, byte[] manifest) throws Exception {
        if(!pack.getName().equals("pack-"+digest(pack)+".zip"))throw new IOException("Pack name/hash mismatch");
        try(RandomAccessFile in=new RandomAccessFile(pack,"r");RandomAccessFile out=new RandomAccessFile(output,"rw")) {
            long length=in.length();if(length<22)throw new IOException("Truncated pack");
            byte[] end=new byte[22];in.seek(length-22);in.readFully(end);
            java.nio.ByteBuffer e=java.nio.ByteBuffer.wrap(end).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            if(e.getInt(0)!=0x06054b50 || e.getShort(20)!=0 || e.getShort(4)!=0 || e.getShort(6)!=0)
                throw new IOException("Unsupported pack ZIP end");
            int count=Short.toUnsignedInt(e.getShort(10));long central=Integer.toUnsignedLong(e.getInt(16));
            long size=Integer.toUnsignedLong(e.getInt(12));
            if(count>=65535 || central+size!=length-22)throw new IOException("ZIP64 carrier required");
            out.setLength(0);byte[] buffer=new byte[65536];in.seek(0);long left=central;
            while(left>0){int n=in.read(buffer,0,(int)Math.min(left,buffer.length));if(n<0)throw new EOFException();out.write(buffer,0,n);left-=n;}
            byte[] name="AndroidManifest.xml".getBytes(StandardCharsets.US_ASCII);CRC32 crc=new CRC32();crc.update(manifest);
            ByteArrayOutputStream local=new ByteArrayOutputStream();
            le(local,0x04034b50,4);le(local,20,2);le(local,0,2);le(local,0,2);le(local,0,2);le(local,33,2);
            le(local,crc.getValue(),4);le(local,manifest.length,4);le(local,manifest.length,4);le(local,name.length,2);le(local,0,2);local.write(name);local.write(manifest);
            out.write(local.toByteArray());long start=out.getFilePointer();left=size;
            while(left>0){int n=in.read(buffer,0,(int)Math.min(left,buffer.length));if(n<0)throw new EOFException();out.write(buffer,0,n);left-=n;}
            ByteArrayOutputStream c=new ByteArrayOutputStream();
            le(c,0x02014b50,4);le(c,0x0314,2);le(c,20,2);le(c,0,2);le(c,0,2);le(c,0,2);le(c,33,2);
            le(c,crc.getValue(),4);le(c,manifest.length,4);le(c,manifest.length,4);le(c,name.length,2);le(c,0,2);
            le(c,0,2);le(c,0,2);le(c,0,2);le(c,0100644L<<16,4);le(c,central,4);c.write(name);out.write(c.toByteArray());
            if(out.getFilePointer()>=0xffffffffL)throw new IOException("ZIP64 carrier required");
            e.putShort(8,(short)(count+1));e.putShort(10,(short)(count+1));e.putInt(12,(int)(size+c.size()));e.putInt(16,(int)start);out.write(end);
        }
    }
    private ObjectPacks() { }
}
