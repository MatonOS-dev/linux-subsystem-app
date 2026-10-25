package org.matonos.linuxhost.stubgen;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.DigestOutputStream;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.BooleanSupplier;

/** Deterministic MTPKIDX3 writer for complete EROFS pack layers. */
public final class ErofsLayers {
    private static final long TARGET=64L*1024*1024;
    private static final int NONE=0xffffffff;
    private ErofsLayers() {}

    public static final class Layer {
        public String split,entry,sha256; public long imageSize;
    }
    public static final class Record {
        public String path,checksum,kind,target=""; public byte[] xattrs=new byte[0];
        public String layerSha; public File source; public int layer=NONE,uid,gid,mode; public long size,mtime,inode;
    }
    public static final class Index {
        public final List<Layer> layers=new ArrayList<>();
        public final List<Record> records=new ArrayList<>();
    }

    private static void le(OutputStream out,long value,int n)throws IOException {
        for(int i=0;i<n;i++){out.write((int)value&255);value>>>=8;}
    }
    private static long readLe(DataInputStream in,int n)throws IOException {
        long v=0;for(int i=0;i<n;i++)v|=(long)in.readUnsignedByte()<<(8*i);return v;
    }
    private static void string(OutputStream out,String value)throws IOException {
        byte[] b=value.getBytes(StandardCharsets.UTF_8);le(out,b.length,4);out.write(b);
    }
    private static String string(DataInputStream in)throws IOException {
        long n=readLe(in,4);if(n>16*1024*1024)throw new IOException("Oversized layers.index string");
        byte[] b=new byte[(int)n];in.readFully(b);return new String(b,StandardCharsets.UTF_8);
    }
    public static Index read(byte[] bytes)throws IOException {
        Index index=new Index();try(DataInputStream in=new DataInputStream(new ByteArrayInputStream(bytes))) {
            byte[] magic=new byte[8];in.readFully(magic);if(!Arrays.equals(magic,"MTPKIDX3".getBytes(StandardCharsets.US_ASCII)))throw new IOException("Invalid MTPKIDX3 magic");
            long count=readLe(in,4);if(count>100000)throw new IOException("Too many EROFS layers");
            for(int i=0;i<count;i++){Layer l=new Layer();l.split=string(in);l.entry=string(in);l.imageSize=readLe(in,8);byte[] h=new byte[32];in.readFully(h);l.sha256=ObjectPacks.hex(h);index.layers.add(l);}
            count=readLe(in,4);if(count>2_000_000)throw new IOException("Too many layer records");
            String previous=null;
            for(int i=0;i<count;i++) {
                Record r=new Record();r.path=string(in);r.checksum=string(in);r.kind=string(in);r.target=string(in);
                long n=readLe(in,4);if(n>16*1024*1024)throw new IOException("Oversized xattrs");r.xattrs=new byte[(int)n];in.readFully(r.xattrs);
                r.layer=(int)readLe(in,4);r.size=readLe(in,8);r.mtime=readLe(in,8);r.uid=(int)readLe(in,4);r.gid=(int)readLe(in,4);r.mode=(int)readLe(in,4);
                if(readLe(in,4)!=0)throw new IOException("Invalid layer record reserved field");r.inode=readLe(in,8);
                if(r.layer!=NONE){if(r.layer<0||r.layer>=index.layers.size())throw new IOException("Invalid layer record reference");r.layerSha=index.layers.get(r.layer).sha256;}
                if(previous!=null&&previous.compareTo(r.path)>=0)throw new IOException("Unsorted layers.index records");previous=r.path;
                index.records.add(r);
            }
            if(in.read()!=-1)throw new IOException("Trailing layers.index bytes");
        } return index;
    }
    public static void write(File output,Index index)throws IOException {
        List<Layer> layers=new ArrayList<>(index.layers);layers.sort(Comparator.comparing(l->l.sha256));
        Map<String,Integer> ids=new HashMap<>();for(int i=0;i<layers.size();i++)ids.put(layers.get(i).sha256,i);
        List<Record> records=new ArrayList<>(index.records);records.sort((a,b)->utf8Compare(a.path,b.path));
        try(OutputStream out=new BufferedOutputStream(new FileOutputStream(output))) {
            out.write("MTPKIDX3".getBytes(StandardCharsets.US_ASCII));le(out,layers.size(),4);
            for(Layer l:layers){string(out,l.split);string(out,l.entry);le(out,l.imageSize,8);out.write(hex(l.sha256));}
            le(out,records.size(),4);String previous=null;
            for(Record r:records){if(previous!=null&&previous.equals(r.path))throw new IOException("Duplicate layer path");previous=r.path;
                string(out,r.path);string(out,r.checksum);string(out,r.kind);string(out,r.target);le(out,r.xattrs.length,4);out.write(r.xattrs);
                le(out,r.layer==NONE?NONE:Objects.requireNonNull(ids.get(r.layerSha),"missing layer reference"),4);
                le(out,r.size,8);le(out,r.mtime,8);le(out,r.uid,4);le(out,r.gid,4);le(out,r.mode,4);le(out,0,4);le(out,r.inode,8);
            }
        }
    }
    private static byte[] hex(String value)throws IOException {if(value==null||!value.matches("[0-9a-f]{64}"))throw new IOException("Invalid layer SHA-256");byte[] b=new byte[32];for(int i=0;i<32;i++)b[i]=(byte)Integer.parseInt(value.substring(i*2,i*2+2),16);return b;}
    private static int utf8Compare(String a,String b){byte[] x=a.getBytes(StandardCharsets.UTF_8),y=b.getBytes(StandardCharsets.UTF_8);for(int i=0;i<Math.min(x.length,y.length);i++){int d=(x[i]&255)-(y[i]&255);if(d!=0)return d;}return x.length-y.length;}

    /** Build changed/new layers, retaining complete images for unchanged records. */
    public static Index build(List<ObjectPacks.Entry> entries,File output,File mkfs,long epoch,
            byte[] previousIndex,Map<String,File> previousImages) throws Exception {
        return build(entries,output,mkfs,epoch,previousIndex,previousImages,()->false);
    }
    public static Index build(List<ObjectPacks.Entry> entries,File output,File mkfs,long epoch,
            byte[] previousIndex,Map<String,File> previousImages,BooleanSupplier cancelled) throws Exception {
        ForkJoinPool pool=new ForkJoinPool(Math.max(1,Runtime.getRuntime().availableProcessors()));
        try { return build(entries,output,mkfs,epoch,previousIndex,previousImages,cancelled,pool); }
        finally { pool.shutdownNow(); }
    }
    public static Index build(List<ObjectPacks.Entry> entries,File output,File mkfs,long epoch,
            byte[] previousIndex,Map<String,File> previousImages,BooleanSupplier cancelled,ForkJoinPool pool) throws Exception {
        if(!output.isDirectory()&&!output.mkdirs())throw new IOException("Cannot create EROFS layers directory");
        Index old=previousIndex==null?new Index():read(previousIndex),result=new Index();
        Map<String,Record> oldRecords=new HashMap<>();for(Record r:old.records)oldRecords.put(r.path,r);
        Map<Integer,Layer> oldLayerByIndex=new HashMap<>();for(int i=0;i<old.layers.size();i++)oldLayerByIndex.put(i,old.layers.get(i));
        Map<String,Record> current=new HashMap<>();List<Record> records=new ArrayList<>();
        for(ObjectPacks.Entry e:entries){Record r=new Record();r.path=e.path;r.checksum=e.checksum;r.kind=e.kind;r.target=e.target;r.xattrs=e.xattrs==null?new byte[0]:e.xattrs.clone();r.uid=e.uid;r.gid=e.gid;r.mode=e.mode;
            if(e.source!=null){r.size=PlaceholderFiles.size(e.source);r.mtime=e.mtime>0?e.mtime:epoch;r.source=e.source;}
            else r.mtime=epoch;
            current.put(r.path,r);records.add(r);
        }
        Map<Integer,long[]> layerBytes=new HashMap<>();
        for(Record before:old.records)if(before.kind.equals("file")) {
            long[] totals=layerBytes.computeIfAbsent(before.layer,k->new long[2]);totals[0]+=before.size;
            Record now=current.get(before.path);
            if(now!=null&&now.kind.equals("file")&&now.checksum.equals(before.checksum)&&now.mode==before.mode&&now.uid==before.uid&&now.gid==before.gid&&Arrays.equals(now.xattrs,before.xattrs))totals[1]+=before.size;
        }
        Set<Integer> rewrite=new HashSet<>();
        for(Map.Entry<Integer,long[]> e:layerBytes.entrySet())if(e.getValue()[0]>e.getValue()[1]
                &&e.getValue()[0]>0&&(double)(e.getValue()[0]-e.getValue()[1])/e.getValue()[0]>=0.5)rewrite.add(e.getKey());
        Map<String,Layer> retained=new HashMap<>();List<Record> changed=new ArrayList<>();
        for(Record r:records)if(r.kind.equals("file")||r.kind.equals("symlink")) {
            Record before=oldRecords.get(r.path);
            if(before!=null&&before.kind.equals(r.kind)&&before.checksum.equals(r.checksum)&&before.target.equals(r.target)&&before.mode==r.mode&&before.uid==r.uid&&before.gid==r.gid&&Arrays.equals(before.xattrs,r.xattrs)) {
                Layer layer=oldLayerByIndex.get(before.layer);File image=previousImages.get(layer==null?"":layer.sha256);
                if(layer!=null&&image!=null&&image.isFile()&&image.length()==layer.imageSize&&sha(image).equals(layer.sha256)) {
                    if(!rewrite.contains(before.layer)) {r.layer=before.layer;r.layerSha=layer.sha256;r.inode=before.inode;retained.put(layer.sha256,layer);continue;}
                }
            }
            changed.add(r);
        }
        Map<String,Integer> newLayerIds=new HashMap<>();
        for(Layer l:retained.values()) {File src=previousImages.get(l.sha256),dst=new File(output,"pack-"+l.sha256+".erofs");if(!src.getCanonicalFile().equals(dst.getCanonicalFile())){try{Files.createLink(dst.toPath(),src.toPath());}catch(IOException|UnsupportedOperationException linkFailure){Files.copy(src.toPath(),dst.toPath(),java.nio.file.StandardCopyOption.REPLACE_EXISTING);}}result.layers.add(l);newLayerIds.put(l.sha256,result.layers.size()-1);}
        for(Record r:records)if(r.layer!=NONE){Layer oldLayer=oldLayerByIndex.get(r.layer);r.layer=newLayerIds.get(oldLayer.sha256);r.layerSha=oldLayer.sha256;}
        List<List<Record>> newBatches=batches(changed);List<ForkJoinTask<LayerBuild>> jobs=new ArrayList<>();
        for(List<Record> batch:newBatches) {
            if(cancelled.getAsBoolean())throw new IOException("EROFS layer build cancelled");
            int mkfsWorkers=Math.max(1,Runtime.getRuntime().availableProcessors()/Math.max(1,pool.getParallelism()));
            jobs.add(pool.submit(()->buildLayer(batch,records,output,mkfs,epoch,cancelled,mkfsWorkers)));
        }
        try {
            for(int i=0;i<newBatches.size();i++) {
                LayerBuild built;
                try { built=jobs.get(i).get(); }
                catch(ExecutionException failure) { Throwable cause=failure.getCause();if(cause instanceof Exception)throw (Exception)cause;throw new IOException("EROFS layer build failed",cause); }
                int id=result.layers.size();result.layers.add(built.layer);
                for(Record r:newBatches.get(i)){r.layer=id;r.layerSha=built.layer.sha256;}
            }
        } finally {
            for(ForkJoinTask<?> job:jobs)if(!job.isDone())job.cancel(true);
        }
        result.records.addAll(records);write(new File(output,"layers.index"),result);return result;
    }
    private static final class LayerBuild { final Layer layer;LayerBuild(Layer layer){this.layer=layer;} }
    private static LayerBuild buildLayer(List<Record> batch,List<Record> records,File output,File mkfs,long epoch,
            BooleanSupplier cancelled,int mkfsWorkers)throws Exception {
        List<Record> imageRecords=new ArrayList<>();Set<String> needed=new HashSet<>();
        for(Record r:batch){String p=r.path;while(!p.equals("/")){int slash=p.lastIndexOf('/');p=slash<=0?"/":p.substring(0,slash);needed.add(p);}}
        for(Record r:records)if(r.kind.equals("dir")&&needed.contains(r.path))imageRecords.add(r);imageRecords.addAll(batch);
        imageRecords.sort((a,b)->utf8Compare(a.path,b.path));
        File image=File.createTempFile("matonos-layer-",".erofs",output);File tarPipe=File.createTempFile("matonos-layer-",".tar.fifo",output);
        File log=File.createTempFile("matonos-layer-",".log",output);
        try {
            if(!tarPipe.delete())throw new IOException("Cannot prepare EROFS tar pipe");
            Process fifo=new ProcessBuilder("mkfifo",tarPipe.getAbsolutePath()).redirectErrorStream(true).start();
            if(fifo.waitFor()!=0)throw new IOException("Cannot create EROFS tar pipe: "+new String(fifo.getInputStream().readAllBytes(),StandardCharsets.UTF_8));
            String mkfsUuid=layerUuid(imageRecords);Process p=new ProcessBuilder(mkfs.getAbsolutePath(),"-zlz4hc,9","--tar=f","--mkfs-time","-T",Long.toString(epoch),"--workers="+mkfsWorkers,"-U",mkfsUuid,image.getAbsolutePath(),tarPipe.getAbsolutePath()).redirectErrorStream(true).redirectOutput(log).start();
            MessageDigest tarIdentity=MessageDigest.getInstance("SHA-256");try(OutputStream tarOut=new DigestOutputStream(new FileOutputStream(tarPipe),tarIdentity)){writeTar(tarOut,imageRecords,epoch);}
            while(!p.waitFor(250,TimeUnit.MILLISECONDS))if(cancelled.getAsBoolean()){p.destroyForcibly();throw new IOException("EROFS layer build cancelled");}
            if(cancelled.getAsBoolean())throw new IOException("EROFS layer build cancelled");
            if(p.exitValue()!=0)throw new IOException("mkfs.erofs failed: "+new String(Files.readAllBytes(log.toPath()),StandardCharsets.UTF_8));
            String digest=sha(image);File dest=new File(output,"pack-"+digest+".erofs");if(dest.exists())image.delete();else Files.move(image.toPath(),dest.toPath());
            dumpLayerInodes(System.getenv().getOrDefault("DUMP_EROFS","dump.erofs"),dest,imageRecords,batch);
            Layer l=new Layer();l.sha256=digest;l.imageSize=dest.length();l.split="pack_"+digest;l.entry="assets/matonos/pack.erofs";
            return new LayerBuild(l);
        } finally {image.delete();tarPipe.delete();log.delete();}
    }
    private static List<List<Record>> batches(List<Record> files){Map<String,List<Record>> dirs=new TreeMap<>();for(Record r:files)dirs.computeIfAbsent(new File(r.path).getParent(),k->new ArrayList<>()).add(r);List<List<Record>> out=new ArrayList<>();List<Record> batch=new ArrayList<>();long size=0;for(List<Record> group:dirs.values()){group.sort((a,b)->utf8Compare(a.path,b.path));for(Record r:group){if(!batch.isEmpty()&&size+r.size>TARGET){out.add(batch);batch=new ArrayList<>();size=0;}batch.add(r);size+=r.size;}}if(!batch.isEmpty())out.add(batch);return out;}
    private static String sha(File file)throws Exception{MessageDigest d=MessageDigest.getInstance("SHA-256");try(InputStream in=new BufferedInputStream(new FileInputStream(file))){byte[] b=new byte[65536];int n;while((n=in.read(b))!=-1)d.update(b,0,n);}return ObjectPacks.hex(d.digest());}
    private static void dumpLayerInodes(String dump,File image,List<Record> imageRecords,List<Record> entries)throws Exception {
        Map<String,Record> byPath=new HashMap<>();for(Record r:entries)byPath.put(r.path,r);
        List<String> directories=new ArrayList<>();directories.add("/");for(Record r:imageRecords)if(r.kind.equals("dir"))directories.add(r.path);
        java.util.regex.Pattern row=java.util.regex.Pattern.compile("^\\s*(\\d+)\\s+\\d+\\s+(.+)$");
        for(String directory:directories) {
            Process query=new ProcessBuilder(dump,"--path="+directory,"--ls",image.getAbsolutePath()).redirectErrorStream(true).start();
            String output=new String(query.getInputStream().readAllBytes(),StandardCharsets.UTF_8);
            if(query.waitFor()!=0)throw new IOException("dump.erofs failed for "+directory+": "+output);
            String base=directory.equals("/")?"":directory;
            for(String line:output.split("\\R")) {
                java.util.regex.Matcher match=row.matcher(line);if(!match.matches())continue;
                String name=match.group(2);if(name.equals(".")||name.equals(".."))continue;
                Record record=byPath.get(base+"/"+name);
                if(record!=null&&(record.kind.equals("file")||record.kind.equals("symlink")))record.inode=Long.parseLong(match.group(1));
            }
        }
        for(Record r:entries)if((r.kind.equals("file")||r.kind.equals("symlink"))&&r.inode==0)
            throw new IOException("dump.erofs returned no NID for "+r.path);
    }
    private static String uuid(String hash){char[] c=hash.substring(0,32).toCharArray();c[12]='5';c[16]="89ab".charAt(Character.digit(c[16],16)&3);return new String(c,0,8)+"-"+new String(c,8,4)+"-"+new String(c,12,4)+"-"+new String(c,16,4)+"-"+new String(c,20,12);}
    private static String layerUuid(List<Record> records)throws Exception {
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
        records.sort((a,b)->utf8Compare(a.path,b.path));try(OutputStream out=new BufferedOutputStream(output)) {int seq=0;for(Record r:records){String name=r.path.equals("/")?".":r.path.substring(1);Map<String,String> pax=new TreeMap<>();if(name.getBytes(StandardCharsets.UTF_8).length>100)pax.put("path",name);if(r.kind.equals("symlink")&&r.target.getBytes(StandardCharsets.UTF_8).length>100)pax.put("linkpath",r.target);if(r.mtime>0)pax.put("mtime",Long.toString(r.mtime));
                Map<String,byte[]> attrs=decodeXattrs(r.xattrs);for(Map.Entry<String,byte[]> x:attrs.entrySet())pax.put("LIBARCHIVE.xattr."+url(x.getKey()),Base64.getEncoder().encodeToString(x.getValue()));
                if(!pax.isEmpty()){String pn="PaxHeaders/"+(seq++);byte[] data=pax(pax);header(out,pn,data.length,0644,0,0,epoch,'x',"");out.write(data);pad(out,data.length);}
                long size=r.kind.equals("file")?r.size:0;char type=r.kind.equals("dir")?'5':r.kind.equals("symlink")?'2':'0';String target=r.kind.equals("symlink")?r.target:"";header(out,name,size,r.mode&07777,r.uid,r.gid,r.mtime>0?r.mtime:epoch,type,target);
                if(r.kind.equals("file")){if(PlaceholderFiles.copy(findSource(r),out)!=size)throw new IOException("layer file changed size: "+r.path);pad(out,size);}
            }out.write(new byte[1024]);}}
    private static File findSource(Record r)throws IOException{if(r.source==null||!r.source.isFile())throw new IOException("missing layer file source: "+r.path);return r.source;}
    private static Map<String,byte[]> decodeXattrs(byte[] raw)throws Exception{return OstreePacks.decodeXattrs(raw);}
    private static String url(String value){try{return java.net.URLEncoder.encode(value,StandardCharsets.UTF_8.name()).replace("+","%20");}catch(Exception e){throw new IllegalStateException(e);}}
    private static byte[] pax(Map<String,String> fields)throws IOException{ByteArrayOutputStream out=new ByteArrayOutputStream();for(Map.Entry<String,String> e:fields.entrySet()){String tail=e.getKey()+"="+e.getValue()+"\n";int n=tail.getBytes(StandardCharsets.UTF_8).length+2;while(true){int next=tail.getBytes(StandardCharsets.UTF_8).length+Integer.toString(n).length()+1;if(next==n)break;n=next;}out.write((Integer.toString(n)+" "+tail).getBytes(StandardCharsets.UTF_8));}return out.toByteArray();}
    private static void header(OutputStream out,String name,long size,int mode,int uid,int gid,long mtime,char type,String link)throws IOException{byte[] h=new byte[512];put(h,0,name,100);oct(h,100,8,mode);oct(h,108,8,uid);oct(h,116,8,gid);oct(h,124,12,size);oct(h,136,12,mtime);Arrays.fill(h,148,156,(byte)' ');h[156]=(byte)type;put(h,157,link,100);put(h,257,"ustar\0",6);put(h,263,"00",2);long sum=0;for(byte b:h)sum+=b&255;String c=String.format(Locale.ROOT,"%06o\0 ",sum);put(h,148,c,8);out.write(h);}
    private static void put(byte[] b,int off,String s,int n){byte[] x=s.getBytes(StandardCharsets.UTF_8);System.arraycopy(x,0,b,off,Math.min(n,x.length));}
    private static void oct(byte[] b,int off,int n,long v){String s=String.format(Locale.ROOT,"%0"+(n-1)+"o",v);put(b,off,s,n-1);b[off+n-1]=0;}
    private static void pad(OutputStream out,long n)throws IOException{int p=(int)((512-(n&511))&511);if(p>0)out.write(new byte[p]);}
}
