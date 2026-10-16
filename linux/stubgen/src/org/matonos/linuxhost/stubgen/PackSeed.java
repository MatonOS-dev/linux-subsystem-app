package org.matonos.linuxhost.stubgen;

import java.io.*;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.*;

/** Materialize canonical file objects in a private bare-user-only scratch repo.
 * Completeness is checked separately by OSTree fsck before enabling static deltas.
 * Installed, signed carriers remain the only payload input. */
public final class PackSeed {
    public static long bytes(List<ObjectPacks.Entry> entries,long blockSize) {
        if(blockSize<=0)throw new IllegalArgumentException("Invalid filesystem block size");
        long size=0;Set<String> seen=new HashSet<>();
        for(ObjectPacks.Entry e:entries)if(eligible(e)&&seen.add(e.checksum))
            size=Math.addExact(size,Math.max(blockSize,Math.multiplyExact(Math.addExact(e.length,blockSize-1)/blockSize,blockSize)));
        return size;
    }
    private static boolean eligible(ObjectPacks.Entry e) {
        return e.uid==0&&e.gid==0&&e.xattrs.length==0&&
                (e.kind.equals("file")&&(e.mode==0100644||e.mode==0100755)||e.kind.equals("symlink")&&e.mode==0120777);
    }
    public static void seed(List<ObjectPacks.Entry> entries,File carriers,File repo,
            java.util.function.BooleanSupplier cancelled)throws Exception {
        for(ObjectPacks.Entry e:entries) {
            if(cancelled.getAsBoolean())throw new IOException("Update cancelled");
            if(!eligible(e))continue;
            Path target=new File(repo,"objects/"+e.checksum.substring(0,2)+"/"+e.checksum.substring(2)+".file").toPath();
            if(Files.exists(target,LinkOption.NOFOLLOW_LINKS))continue;
            Files.createDirectories(target.getParent());
            if(e.kind.equals("symlink")){Files.createSymbolicLink(target,Path.of(e.target));continue;}
            Path temp=Files.createTempFile(target.getParent(),"seed-",".tmp");
            try(RandomAccessFile input=new RandomAccessFile(new File(carriers,e.pack),"r");
                    OutputStream output=Files.newOutputStream(temp)) {
                if(e.offset>input.length()-e.length)throw new IOException("Truncated installed pack");
                input.seek(e.offset);byte[] buffer=new byte[65536];long left=e.length;
                while(left>0){if(cancelled.getAsBoolean())throw new IOException("Update cancelled");
                    int n=input.read(buffer,0,(int)Math.min(left,buffer.length));if(n<0)throw new EOFException();output.write(buffer,0,n);left-=n;}
            }catch(Exception error){Files.deleteIfExists(temp);throw error;}
            try {
                Files.setPosixFilePermissions(temp,PosixFilePermissions.fromString(e.mode==0100755?"rwxr-xr-x":"rw-r--r--"));
                Files.move(temp,target,StandardCopyOption.ATOMIC_MOVE);
            }finally{Files.deleteIfExists(temp);}
        }
    }
    private PackSeed(){}
}
