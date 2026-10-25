package org.matonos.linuxhost.stubgen;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.DigestOutputStream;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.BooleanSupplier;

/** Deterministic single-image builder for one complete package deployment. */
public final class ErofsImage {
    private ErofsImage() {}

    public static final class Image {
        public String split,entry,sha256; public long imageSize;
    }
    public static final class Record {
        public String path,checksum,kind,target=""; public byte[] xattrs=new byte[0];
        public File source; public int uid,gid,mode; public long size,mtime;
    }
    public static final class BuildResult {
        public Image image;
    }

    private static int utf8Compare(String a,String b){byte[] x=a.getBytes(StandardCharsets.UTF_8),y=b.getBytes(StandardCharsets.UTF_8);for(int i=0;i<Math.min(x.length,y.length);i++){int d=(x[i]&255)-(y[i]&255);if(d!=0)return d;}return x.length-y.length;}

    /** Build one complete package image from its staged deployment. */
    public static BuildResult build(List<ObjectPacks.Entry> entries,File output,File mkfs,long epoch) throws Exception {
        ForkJoinPool pool=new ForkJoinPool(Math.max(1,Runtime.getRuntime().availableProcessors()));
        try { return build(entries,output,mkfs,epoch,()->false,pool); }
        finally { pool.shutdownNow(); }
    }
    public static BuildResult build(List<ObjectPacks.Entry> entries,File output,File mkfs,long epoch,
            BooleanSupplier cancelled,ForkJoinPool pool) throws Exception {
        if(!output.isDirectory()&&!output.mkdirs())throw new IOException("Cannot create EROFS image directory");
        BuildResult result=new BuildResult();List<Record> records=new ArrayList<>();
        for(ObjectPacks.Entry e:entries){Record r=new Record();r.path=e.path;r.checksum=e.checksum;r.kind=e.kind;r.target=e.target;r.xattrs=e.xattrs==null?new byte[0]:e.xattrs.clone();r.uid=e.uid;r.gid=e.gid;r.mode=e.mode;
            if(e.source!=null){r.size=PlaceholderFiles.size(e.source);r.mtime=e.mtime>0?e.mtime:epoch;r.source=e.source;}
            else r.mtime=epoch;
            records.add(r);
        }
        if(cancelled.getAsBoolean())throw new IOException("EROFS image build cancelled");
        int workers=Math.max(1,Runtime.getRuntime().availableProcessors());
        result.image=buildImage(records,output,mkfs,epoch,cancelled,workers);return result;
    }
    private static Image buildImage(List<Record> records,File output,File mkfs,long epoch,
            BooleanSupplier cancelled,int mkfsWorkers)throws Exception {
        List<Record> imageRecords=new ArrayList<>(records);
        imageRecords.sort((a,b)->utf8Compare(a.path,b.path));
        File image=File.createTempFile("matonos-image-",".erofs",output);File tarPipe=File.createTempFile("matonos-image-",".tar.fifo",output);
        File log=File.createTempFile("matonos-image-",".log",output);
        try {
            if(!tarPipe.delete())throw new IOException("Cannot prepare EROFS tar pipe");
            Process fifo=new ProcessBuilder("mkfifo",tarPipe.getAbsolutePath()).redirectErrorStream(true).start();
            if(fifo.waitFor()!=0)throw new IOException("Cannot create EROFS tar pipe: "+new String(fifo.getInputStream().readAllBytes(),StandardCharsets.UTF_8));
            String mkfsUuid=deterministicUuid(imageRecords);Process p=new ProcessBuilder(mkfs.getAbsolutePath(),"-zlz4hc,9","-Ededupe","--tar=f","--mkfs-time","-T",Long.toString(epoch),"--workers="+mkfsWorkers,"-U",mkfsUuid,image.getAbsolutePath(),tarPipe.getAbsolutePath()).redirectErrorStream(true).redirectOutput(log).start();
            MessageDigest tarIdentity=MessageDigest.getInstance("SHA-256");try(OutputStream tarOut=new DigestOutputStream(new FileOutputStream(tarPipe),tarIdentity)){writeTar(tarOut,imageRecords,epoch);}
            while(!p.waitFor(250,TimeUnit.MILLISECONDS))if(cancelled.getAsBoolean()){p.destroyForcibly();throw new IOException("EROFS image build cancelled");}
            if(cancelled.getAsBoolean())throw new IOException("EROFS image build cancelled");
            if(p.exitValue()!=0)throw new IOException("mkfs.erofs failed: "+new String(PlaceholderFiles.readAllBytes(log),StandardCharsets.UTF_8));
            String digest=sha(image);File dest=new File(output,"deployment.erofs");Files.move(image.toPath(),dest.toPath(),java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            Image l=new Image();l.sha256=digest;l.imageSize=dest.length();l.split="erofs_"+digest;l.entry="assets/matonos/deployment.erofs";
            return l;
        } finally {image.delete();tarPipe.delete();log.delete();}
    }
    private static String sha(File file)throws Exception{MessageDigest d=MessageDigest.getInstance("SHA-256");try(InputStream in=new BufferedInputStream(new FileInputStream(file))){byte[] b=new byte[65536];int n;while((n=in.read(b))!=-1)d.update(b,0,n);}return ObjectPacks.hex(d.digest());}
    private static String uuid(String hash){char[] c=hash.substring(0,32).toCharArray();c[12]='5';c[16]="89ab".charAt(Character.digit(c[16],16)&3);return new String(c,0,8)+"-"+new String(c,8,4)+"-"+new String(c,12,4)+"-"+new String(c,16,4)+"-"+new String(c,20,12);}
    private static String deterministicUuid(List<Record> records)throws Exception {
        MessageDigest digest=MessageDigest.getInstance("SHA-256");
        for(Record r:records) {
            for(String value:new String[]{r.path,r.checksum,r.kind,r.target}) {
                byte[] bytes=value.getBytes(StandardCharsets.UTF_8);updateLe(digest,bytes.length,4);digest.update(bytes);
            }
            updateLe(digest,r.xattrs.length,4);digest.update(r.xattrs);
            updateLe(digest,r.size,8);updateLe(digest,r.mtime,8);updateLe(digest,r.uid,4);updateLe(digest,r.gid,4);updateLe(digest,r.mode,4);
        }
        return uuid(ObjectPacks.hex(digest.digest()));
    }
    private static void updateLe(MessageDigest digest,long value,int bytes) {
        for(int i=0;i<bytes;i++){digest.update((byte)value);value>>>=8;}
    }

    private static void writeTar(OutputStream output,List<Record> records,long epoch)throws Exception {
        records.sort((a,b)->utf8Compare(a.path,b.path));try(OutputStream out=new BufferedOutputStream(output)) {int seq=0;Map<String,String> objectNames=new HashMap<>();for(Record r:records){String name=r.path.equals("/")?".":r.path.substring(1);boolean identified=r.checksum!=null&&!r.checksum.isEmpty();String hardlink=r.kind.equals("file")&&identified?objectNames.get(r.checksum):null;Map<String,String> pax=new TreeMap<>();if(name.getBytes(StandardCharsets.UTF_8).length>100)pax.put("path",name);String linkTarget=r.kind.equals("symlink")?r.target:hardlink;if(linkTarget!=null&&linkTarget.getBytes(StandardCharsets.UTF_8).length>100)pax.put("linkpath",linkTarget);if(r.mtime>0)pax.put("mtime",Long.toString(r.mtime));
                Map<String,byte[]> attrs=decodeXattrs(r.xattrs);for(Map.Entry<String,byte[]> x:attrs.entrySet())pax.put("LIBARCHIVE.xattr."+url(x.getKey()),Base64.getEncoder().encodeToString(x.getValue()));
                if(!pax.isEmpty()){String pn="PaxHeaders/"+(seq++);byte[] data=pax(pax);header(out,pn,data.length,0644,0,0,epoch,'x',"");out.write(data);pad(out,data.length);}
                long size=r.kind.equals("file")&&hardlink==null?r.size:0;char type=r.kind.equals("dir")?'5':r.kind.equals("symlink")?'2':hardlink!=null?'1':'0';String target=linkTarget==null?"":linkTarget;header(out,name,size,r.mode&07777,r.uid,r.gid,r.mtime>0?r.mtime:epoch,type,target);
                if(r.kind.equals("file")&&hardlink==null){if(identified)objectNames.put(r.checksum,name);if(PlaceholderFiles.copy(findSource(r),out)!=size)throw new IOException("image file changed size: "+r.path);pad(out,size);}
            }out.write(new byte[1024]);}}
    private static File findSource(Record r)throws IOException{if(r.source==null||!r.source.isFile())throw new IOException("missing image file source: "+r.path);return r.source;}
    private static Map<String,byte[]> decodeXattrs(byte[] raw)throws Exception{return OstreePacks.decodeXattrs(raw);}
    private static String url(String value){try{return java.net.URLEncoder.encode(value,StandardCharsets.UTF_8.name()).replace("+","%20");}catch(Exception e){throw new IllegalStateException(e);}}
    private static byte[] pax(Map<String,String> fields)throws IOException{ByteArrayOutputStream out=new ByteArrayOutputStream();for(Map.Entry<String,String> e:fields.entrySet()){String tail=e.getKey()+"="+e.getValue()+"\n";int n=tail.getBytes(StandardCharsets.UTF_8).length+2;while(true){int next=tail.getBytes(StandardCharsets.UTF_8).length+Integer.toString(n).length()+1;if(next==n)break;n=next;}out.write((Integer.toString(n)+" "+tail).getBytes(StandardCharsets.UTF_8));}return out.toByteArray();}
    private static void header(OutputStream out,String name,long size,int mode,int uid,int gid,long mtime,char type,String link)throws IOException{byte[] h=new byte[512];put(h,0,name,100);oct(h,100,8,mode);oct(h,108,8,uid);oct(h,116,8,gid);oct(h,124,12,size);oct(h,136,12,mtime);Arrays.fill(h,148,156,(byte)' ');h[156]=(byte)type;put(h,157,link,100);put(h,257,"ustar\0",6);put(h,263,"00",2);long sum=0;for(byte b:h)sum+=b&255;String c=String.format(Locale.ROOT,"%06o\0 ",sum);put(h,148,c,8);out.write(h);}
    private static void put(byte[] b,int off,String s,int n){byte[] x=s.getBytes(StandardCharsets.UTF_8);System.arraycopy(x,0,b,off,Math.min(n,x.length));}
    private static void oct(byte[] b,int off,int n,long v){String s=String.format(Locale.ROOT,"%0"+(n-1)+"o",v);put(b,off,s,n-1);b[off+n-1]=0;}
    private static void pad(OutputStream out,long n)throws IOException{int p=(int)((512-(n&511))&511);if(p>0)out.write(new byte[p]);}
}
