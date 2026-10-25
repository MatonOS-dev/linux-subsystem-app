package org.matonos.linuxhost.stubgen;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Writer for the MTPKIDX2 merged deployment index produced by erofs_layers.py. */
public final class ErofsLayers {
    private ErofsLayers() {}
    public static final class Layer {
        public String split, entry;
        public long imageSize;
        public byte[] sha256;
        public Layer(String split,String entry,long imageSize,byte[] sha256) {
            this.split=split;this.entry=entry;this.imageSize=imageSize;this.sha256=sha256;
        }
    }
    public static final class Entry {
        public String path,checksum,kind,target="";
        public byte[] xattrs=new byte[0];
        public int layer=-1,uid,gid,mode;
        public long size,mtime;
    }
    private static void le(OutputStream out,long value,int width)throws IOException {
        for(int n=0;n<width;n++){out.write((int)value&255);value>>>=8;}
    }
    private static void string(OutputStream out,String value)throws IOException {
        byte[] bytes=value.getBytes(StandardCharsets.UTF_8);le(out,bytes.length,4);out.write(bytes);
    }
    private static final Comparator<String> UTF8=(a,b)->{
        byte[] x=a.getBytes(StandardCharsets.UTF_8),y=b.getBytes(StandardCharsets.UTF_8);
        for(int i=0;i<Math.min(x.length,y.length);i++){int d=(x[i]&255)-(y[i]&255);if(d!=0)return d;}
        return x.length-y.length;
    };
    public static void writeIndex(File output,List<Layer> layers,List<Entry> entries)throws IOException {
        if(layers.size()>0xffffffffL||entries.size()>0xffffffffL)throw new IOException("MTPKIDX2 count overflow");
        Set<String> layerNames=new HashSet<>();
        for(Layer l:layers)if(l.split==null||!l.split.matches("pack_[0-9a-f]{64}")||l.entry==null||
                !l.entry.equals("assets/matonos/pack.erofs")||l.imageSize<=0||l.sha256==null||l.sha256.length!=32||
                !layerNames.add(l.split))throw new IOException("Invalid EROFS layer table entry");
        List<Entry> sorted=new ArrayList<>(entries);sorted.sort((a,b)->UTF8.compare(a.path,b.path));
        try(OutputStream out=new BufferedOutputStream(new FileOutputStream(output))) {
            out.write("MTPKIDX2".getBytes(StandardCharsets.US_ASCII));le(out,layers.size(),4);
            for(Layer l:layers){string(out,l.split);string(out,l.entry);le(out,l.imageSize,8);out.write(l.sha256);}
            le(out,sorted.size(),4);String previous=null;
            for(Entry e:sorted){
                if(e.path==null||!e.path.startsWith("/")||e.path.equals(previous)||e.checksum==null||
                        !e.checksum.matches("[0-9a-f]{64}")||e.kind==null||e.xattrs==null||
                        e.uid<0||e.gid<0||e.mode<0||e.size<0||e.mtime<0||
                        (e.kind.equals("file")?(e.layer<0||e.layer>=layers.size()):(e.layer!=-1)))
                    throw new IOException("Invalid MTPKIDX2 entry");
                previous=e.path;string(out,e.path);string(out,e.checksum);string(out,e.kind);string(out,e.target==null?"":e.target);
                le(out,e.xattrs.length,4);out.write(e.xattrs);le(out,e.layer<0?0xffffffffL:e.layer,4);
                le(out,e.size,8);le(out,e.mtime,8);le(out,e.uid,4);le(out,e.gid,4);le(out,e.mode,4);le(out,0,4);
            }
        }
    }
}
