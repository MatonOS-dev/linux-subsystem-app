package org.matonos.linux.stub;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermission;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.EnumSet;
import java.util.List;
import java.util.Set;
import org.matonos.linuxhost.stubgen.PlaceholderFiles;

/** Ustar transport for isolated Flatpak deploy inputs and /app/extra output. */
final class TarStream {
    private static final int BLOCK=512;
    private TarStream() { }

    /** Streams staging and a materialized repo as ordinary files into the isolated UID's bwrap tmpfs. */
    static void writeDeployment(Path installation,Path repo,OutputStream destination) throws IOException {
        OutputStream out=new BufferedOutputStream(destination);
        writeTree(installation,"install",out,true);
        writeTree(repo,"repo",out,false);
        out.write(new byte[BLOCK*2]);out.flush();
    }

    private static void writeTree(Path root,String name,OutputStream out,boolean staging) throws IOException {
        header(out,name+"/",0755,0,'5',"");
        List<Path> entries=new ArrayList<>();
        try(java.util.stream.Stream<Path> paths=Files.walk(root)) { paths.forEach(entries::add); }
        entries.sort(Comparator.comparing(path->root.relativize(path).toString()));
        for(Path path:entries) {
            Path relative=root.relativize(path);String rel=relative.toString().replace(java.io.File.separatorChar,'/');
            if(rel.isEmpty()||staging&&(rel.equals("repo")||rel.startsWith("repo/")||rel.equals("isolated-repo")
                    ||rel.startsWith("isolated-repo/")||rel.equals(".v3-store-root")))continue;
            if(staging&&rel.startsWith(".v3-inodes/"))continue;
            String entry=name+"/"+rel;boolean symlink=Files.isSymbolicLink(path);
            if(Files.isDirectory(path,LinkOption.NOFOLLOW_LINKS))header(out,entry.endsWith("/")?entry:entry+"/",0755,0,'5',"");
            else if(symlink) {
                String target=Files.readSymbolicLink(path).toString();
                header(out,entry,0777,0,'2',target);
            } else if(Files.isRegularFile(path,LinkOption.NOFOLLOW_LINKS)) {
                long size=Files.size(path);int permissions=(mode(path,0644)&0111)!=0?0755:0644;
                header(out,entry,permissions,size,'0',"");
                try(InputStream in=PlaceholderFiles.open(path.toFile())) { copy(in,out,size); }
                pad(out,size);
            } else throw new IOException("Unsupported staged Flatpak file: "+entry);
        }
    }

    static void writeExtra(Path extra,OutputStream destination) throws IOException {
        OutputStream out=new BufferedOutputStream(destination);
        header(out,"extra",0755,0,'5',"");
        if(Files.exists(extra,LinkOption.NOFOLLOW_LINKS)) {
            List<Path> entries=new ArrayList<>();
            try(java.util.stream.Stream<Path> paths=Files.walk(extra)) { paths.forEach(entries::add); }
            entries.sort(Comparator.comparing(Path::toString));
            for(Path path:entries) {
                String relative=extra.relativize(path).toString().replace(java.io.File.separatorChar,'/');
                if(relative.isEmpty())continue;
                String name="extra/"+relative; boolean symlink=Files.isSymbolicLink(path);
                if(Files.isDirectory(path,LinkOption.NOFOLLOW_LINKS)) {
                    header(out,name.endsWith("/")?name:name+"/",0755,0,'5',"");
                } else if(symlink) {
                    String target=Files.readSymbolicLink(path).toString();
                    if(target.length()>100)throw new IOException("Extra-data symlink target is too long for ustar");
                    header(out,name,0777,0,'2',target);
                } else if(Files.isRegularFile(path,LinkOption.NOFOLLOW_LINKS)) {
                    long size=Files.size(path);int permissions=(mode(path,0644)&0111)!=0?0755:0644;
                    header(out,name,permissions,size,'0',"");
                    try(InputStream in=PlaceholderFiles.open(path.toFile())){copy(in,out,size);}
                    pad(out,size);
                } else throw new IOException("Unsupported file in apply_extra output: "+name);
            }
        }
        out.write(new byte[BLOCK*2]);out.flush();
    }

    /** Imports only regular files, directories and safe relative symlinks under files/extra. */
    static void extractExtra(InputStream source,Path files) throws IOException {
        DataInputStream in=new DataInputStream(new BufferedInputStream(source));
        Path root=files.resolve("extra").normalize();
        byte[] block=new byte[BLOCK];boolean ended=false;
        while(!ended) {
            readFully(in,block);boolean zero=true;for(byte b:block)if(b!=0){zero=false;break;}
            if(zero) {
                readFully(in,block);for(byte b:block)if(b!=0)throw new IOException("Invalid tar end marker");
                ended=true;break;
            }
            verifyHeader(block);
            String name=field(block,0,100),prefix=field(block,345,155);
            if(!prefix.isEmpty())name=prefix+"/"+name;
            String link=field(block,157,100);long size=octal(block,124,12),mode=octal(block,100,8);
            char type=(char)block[156];
            if(!name.equals("extra")&&!name.startsWith("extra/"))throw new IOException("Tar path leaves the apply_extra directory");
            String suffix=name.equals("extra")?"":name.substring(6);
            Path target=suffix.isEmpty()?root:root.resolve(suffix).normalize();
            if(!target.equals(root)&&!target.startsWith(root))throw new IOException("Tar path traversal");
            requireRealParents(root,target);
            if(!target.equals(root)&&Files.isSymbolicLink(target))throw new IOException("Tar entry replaces a symlink");
            if(type=='5') {
                if(size!=0)throw new IOException("Directory tar entry has data");
                Files.createDirectories(target);setMode(target,0755);
            } else if(type=='0'||type==0) {
                if(size<0||size>Integer.MAX_VALUE)throw new IOException("Tar entry too large");
                Files.createDirectories(target.getParent());
                try(OutputStream out=new BufferedOutputStream(Files.newOutputStream(target))) {copy(in,out,size);}
                pad(in,size);setMode(target,((int)mode&0111)!=0?0755:0644);
            } else if(type=='2') {
                if(size!=0||link.isEmpty())throw new IOException("Invalid symlink tar entry");
                Path relative=Path.of(link);
                Files.createDirectories(target.getParent());Files.createSymbolicLink(target,relative);
            } else throw new IOException("Unsupported tar entry type: "+(int)type);
        }
        if(!ended)throw new EOFException("Truncated tar stream");
        if(in.read()!=-1)throw new IOException("Trailing bytes after tar stream");
    }

    /** Archive entries may preserve stock symlinks, but never write through one. */
    private static void requireRealParents(Path root,Path target)throws IOException {
        for(Path parent=target.getParent();parent!=null&&parent.startsWith(root);parent=parent.getParent())
            if(Files.isSymbolicLink(parent))throw new IOException("Tar entry parent is a symlink");
    }

    private static void header(OutputStream out,String rawName,int mode,long size,char type,String link) throws IOException {
        byte[] h=new byte[BLOCK];String name=rawName,prefix="";
        if(name.length()>100) {
            int split=-1;for(int i=name.lastIndexOf('/');i>0;i=name.lastIndexOf('/',i-1))
                if(name.substring(0,i).length()<=155&&name.substring(i+1).length()<=100){split=i;break;}
            if(split<0)throw new IOException("Extra-data path is too long for ustar");
            prefix=name.substring(0,split);name=name.substring(split+1);
        }
        put(h,0,100,name);putOctal(h,100,8,mode);putOctal(h,108,8,0);putOctal(h,116,8,0);
        putOctal(h,124,12,size);putOctal(h,136,12,0);for(int i=148;i<156;i++)h[i]=32;
        h[156]=(byte)type;put(h,157,100,link);put(h,257,6,"ustar\0");put(h,263,2,"00");put(h,345,155,prefix);
        long sum=0;for(byte b:h)sum+=b&255;putOctal(h,148,8,sum);
        out.write(h);
    }
    private static void verifyHeader(byte[] h) throws IOException {
        long expected=octal(h,148,8),actual=0;for(int i=0;i<BLOCK;i++)actual+=(i>=148&&i<156)?32:h[i]&255;
        if(expected!=actual||!field(h,257,5).equals("ustar"))throw new IOException("Invalid ustar header");
        // UIDs, GIDs and names are informational only; extractExtra never applies them.
        for(int i=500;i<BLOCK;i++)if(h[i]!=0)throw new IOException("Tar extension metadata is not accepted");
    }
    private static int mode(Path path,int fallback) {
        try {Set<PosixFilePermission> p=Files.getPosixFilePermissions(path,LinkOption.NOFOLLOW_LINKS);int m=0;
            if(p.contains(PosixFilePermission.OWNER_READ))m|=0400;if(p.contains(PosixFilePermission.OWNER_WRITE))m|=0200;if(p.contains(PosixFilePermission.OWNER_EXECUTE))m|=0100;
            if(p.contains(PosixFilePermission.GROUP_READ))m|=0040;if(p.contains(PosixFilePermission.GROUP_WRITE))m|=0020;if(p.contains(PosixFilePermission.GROUP_EXECUTE))m|=0010;
            if(p.contains(PosixFilePermission.OTHERS_READ))m|=0004;if(p.contains(PosixFilePermission.OTHERS_WRITE))m|=0002;if(p.contains(PosixFilePermission.OTHERS_EXECUTE))m|=0001;return m;
        }catch(Exception ignored){return fallback;}
    }
    private static void setMode(Path path,int mode) throws IOException {
        EnumSet<PosixFilePermission> p=EnumSet.noneOf(PosixFilePermission.class);
        if((mode&0400)!=0)p.add(PosixFilePermission.OWNER_READ);if((mode&0200)!=0)p.add(PosixFilePermission.OWNER_WRITE);if((mode&0100)!=0)p.add(PosixFilePermission.OWNER_EXECUTE);
        if((mode&0040)!=0)p.add(PosixFilePermission.GROUP_READ);if((mode&0020)!=0)p.add(PosixFilePermission.GROUP_WRITE);if((mode&0010)!=0)p.add(PosixFilePermission.GROUP_EXECUTE);
        if((mode&0004)!=0)p.add(PosixFilePermission.OTHERS_READ);if((mode&0002)!=0)p.add(PosixFilePermission.OTHERS_WRITE);if((mode&0001)!=0)p.add(PosixFilePermission.OTHERS_EXECUTE);
        Files.setPosixFilePermissions(path,p);
    }
    private static void put(byte[] b,int off,int len,String value) throws IOException {
        byte[] bytes=value.getBytes(StandardCharsets.UTF_8);if(bytes.length>len)throw new IOException("Tar field too long");System.arraycopy(bytes,0,b,off,bytes.length);
    }
    private static void putOctal(byte[] b,int off,int len,long value) throws IOException {
        String text=Long.toOctalString(value);if(text.length()>len-1)throw new IOException("Tar number too large");
        for(int i=0;i<len-1;i++)b[off+i]=(byte)'0';byte[] digits=text.getBytes(StandardCharsets.US_ASCII);System.arraycopy(digits,0,b,off+len-1-digits.length,digits.length);b[off+len-1]=0;
    }
    private static long octal(byte[] b,int off,int len) throws IOException {
        long value=0;int i=off,end=off+len;while(i<end&&(b[i]==0||b[i]==' '))i++;
        for(;i<end&&b[i]!=0&&b[i]!=' ';i++){if(b[i]<'0'||b[i]>'7')throw new IOException("Invalid tar number");value=(value<<3)+(b[i]-'0');}
        return value;
    }
    private static String field(byte[] b,int off,int len) {int end=off;while(end<off+len&&b[end]!=0)end++;return new String(b,off,end-off,StandardCharsets.UTF_8);}
    private static void copy(InputStream in,OutputStream out,long size) throws IOException {
        byte[] b=new byte[65536];long left=size;while(left>0){int n=in.read(b,0,(int)Math.min(left,b.length));if(n<0)throw new EOFException("Truncated tar file");out.write(b,0,n);left-=n;}
    }
    private static void pad(OutputStream out,long size) throws IOException {int n=(int)((BLOCK-(size%BLOCK))%BLOCK);if(n!=0)out.write(new byte[n]);}
    private static void pad(InputStream in,long size) throws IOException {int n=(int)((BLOCK-(size%BLOCK))%BLOCK);while(n>0){long skipped=in.skip(n);if(skipped<=0){if(in.read()<0)throw new EOFException("Truncated tar padding");skipped=1;}n-=skipped;}}
    private static void readFully(InputStream in,byte[] b) throws IOException {int at=0;while(at<b.length){int n=in.read(b,at,b.length-at);if(n<0)throw new EOFException("Truncated tar stream");at+=n;}}
}
