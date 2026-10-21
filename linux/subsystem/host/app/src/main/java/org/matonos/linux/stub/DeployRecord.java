package org.matonos.linux.stub;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;

/** Writes the Flatpak deploy record (ssasta{sv}) for a runtime extension, matching gl-maton/build.py. */
final class DeployRecord {
    private DeployRecord() {}

    static byte[] extension(String origin,String commit,String runtimeBranch) {
        String runtime="org.freedesktop.Platform/x86_64/"+runtimeBranch;
        byte[][] entries={
                entry("deploy-version",variant(le(4,4),"i")),
                entry("timestamp",variant(le(0,8),"t")),
                entry("runtime",variant(cstring(runtime),"s")),
                entry("extension-of",variant(cstring("runtime/"+runtime),"s"))};
        Buffer dict=new Buffer();long[] ends=new long[entries.length];
        for(int i=0;i<entries.length;i++){dict.align(8);dict.put(entries[i]);ends[i]=dict.size();}
        byte[] metadata=dict.frame(ends);
        Buffer tuple=new Buffer();
        tuple.put(cstring(origin));long originEnd=tuple.size();
        tuple.put(cstring(commit));long commitEnd=tuple.size();
        long subpathsEnd=tuple.size(); // empty as
        tuple.align(8);tuple.put(le(0,8));
        tuple.align(8);tuple.put(metadata);
        // Tuple framing offsets are stored last member first.
        return tuple.frame(new long[]{subpathsEnd,commitEnd,originEnd});
    }
    private static byte[] entry(String key,byte[] value) {
        Buffer entry=new Buffer();entry.put(cstring(key));long keyEnd=entry.size();
        entry.align(8);entry.put(value);
        return entry.frame(new long[]{keyEnd});
    }
    private static byte[] variant(byte[] value,String type) {
        Buffer variant=new Buffer();variant.put(value);variant.out.write(0);
        variant.put(type.getBytes(StandardCharsets.US_ASCII));
        return variant.out.toByteArray();
    }
    private static byte[] cstring(String value) {
        byte[] bytes=value.getBytes(StandardCharsets.UTF_8),out=new byte[bytes.length+1];
        System.arraycopy(bytes,0,out,0,bytes.length);return out;
    }
    private static byte[] le(long value,int width) {
        byte[] out=new byte[width];for(int i=0;i<width;i++)out[i]=(byte)(value>>>(8*i));return out;
    }
    private static final class Buffer {
        final ByteArrayOutputStream out=new ByteArrayOutputStream();
        long size(){return out.size();}
        void put(byte[] bytes){out.write(bytes,0,bytes.length);}
        void align(int to){while(out.size()%to!=0)out.write(0);}
        /** Appends framing offsets using the smallest width that addresses the whole container. */
        byte[] frame(long[] offsets) {
            long body=size();int width=0;
            if(body+offsets.length>0)
                for(width=1;width<8&&body+(long)width*offsets.length>(1L<<(8*width))-1;width*=2){}
            for(long offset:offsets)put(le(offset,width));
            return out.toByteArray();
        }
    }
}
