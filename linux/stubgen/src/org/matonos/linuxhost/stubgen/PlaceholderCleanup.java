package org.matonos.linuxhost.stubgen;

import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;

/** Native Java-tree cleanup shares the proxy's flock, never its fcntl lock. */
public final class PlaceholderCleanup {
    private PlaceholderCleanup() {}
    private static final class Native {
        static { System.loadLibrary("maton_link_gc"); }
        static native int lock(String store) throws IOException;
        static native void unlock(int fd);
    }
    private static String key(PlaceholderFiles.Stat s) {
        return String.format(Locale.ROOT,"p-%016x-%016x",s.dev,s.ino);
    }
    private static boolean exists(File f) { return Files.exists(f.toPath(),LinkOption.NOFOLLOW_LINKS); }
    private static File[] children(File f) throws IOException {
        File[] result=f.listFiles();
        if(result==null)throw new IOException("Cannot list "+f);
        return result;
    }
    private static void count(File store,String id,long value) throws IOException {
        File object=new File(store,id), side=new File(store,id+".count");
        if(value==0) { Files.deleteIfExists(object.toPath()); Files.deleteIfExists(side.toPath()); }
        else Files.write(side.toPath(),ByteBuffer.allocate(8).order(ByteOrder.nativeOrder()).putLong(value).array());
    }
    /** Removes a tree without following symlinks; failures leave recovery to the sweep. */
    public static synchronized void deleteTree(File tree) throws IOException {
        PlaceholderFiles.Stat s=PlaceholderFiles.backend().lstat(tree.getPath());
        if(s==null)return;
        if(s.directory()) {
            tree.setWritable(true,true);
            for(File child:children(tree))deleteTree(child);
            Files.delete(tree.toPath());
            return;
        }
        PlaceholderFiles.Store store=s.regular()&&s.sticky()&&s.uid==PlaceholderFiles.backend().uid()?PlaceholderFiles.findStore(tree):null;
        if(store==null) { Files.delete(tree.toPath()); return; }
        int fd=Native.lock(store.store.getPath());
        try {
            if(exists(new File(store.store,"intent")))throw new IOException("Pending compat-proxy hardlink intent: "+store.store);
            // Re-read identity and index after acquiring the proxy lock.
            s=PlaceholderFiles.backend().lstat(tree.getPath());
            if(s==null)return;
            String id=s.regular()&&s.sticky()?store.backingId(s):null;
            long links=0;
            if(id!=null) {
                byte[] raw=Files.readAllBytes(new File(store.store,id+".count").toPath());
                if(raw.length!=8)throw new IOException("Invalid hardlink count: "+tree);
                links=ByteBuffer.wrap(raw).order(ByteOrder.nativeOrder()).getLong();
            }
            if(id!=null&&links==0)throw new IOException("Invalid hardlink count: "+tree);
            Files.delete(tree.toPath());
            if(id!=null) {
                Files.deleteIfExists(new File(store.store,key(s)).toPath());
                count(store.store,id,links-1);
            }
        } finally { Native.unlock(fd); }
    }
    /** One same-device walk. A pending proxy journal must be replayed by the proxy. */
    public static synchronized void sweep(File root) throws IOException {
        File store=new File(root,".v3-inodes");
        if(!exists(store))return;
        PlaceholderFiles.Stat base=PlaceholderFiles.backend().lstat(root.getPath());
        PlaceholderFiles.Stat dir=PlaceholderFiles.backend().lstat(store.getPath());
        if(base==null||dir==null||!dir.directory()||dir.uid!=PlaceholderFiles.backend().uid()
                ||dir.dev!=base.dev||(dir.mode&0077)!=0)throw new IOException("Invalid hardlink store: "+store);
        int fd=Native.lock(store.getPath());
        try {
            if(exists(new File(store,"intent")))return;
            Set<String> live=new HashSet<>();
            collect(root,base.dev,live);
            Map<String,Long> counts=new HashMap<>();
            PlaceholderFiles.Store reader=new PlaceholderFiles.Store(store);
            for(File entry:children(store)) {
                String name=entry.getName();
                if(!name.matches("p-[0-9a-f]{16}-[0-9a-f]{16}"))continue;
                if(!live.contains(name)) { Files.delete(entry.toPath()); continue; }
                String[] parts=name.split("-");
                PlaceholderFiles.Stat identity=new PlaceholderFiles.Stat(Long.parseUnsignedLong(parts[1],16),
                        Long.parseUnsignedLong(parts[2],16),0101000,PlaceholderFiles.backend().uid());
                String id=reader.backingId(identity);
                if(id!=null)counts.put(id,counts.getOrDefault(id,0L)+1);
            }
            Set<String> objects=new HashSet<>();
            for(File entry:children(store)) {
                String name=entry.getName();
                if(name.matches("i-[0-9a-f]{32}(\\.count)?"))objects.add(name.substring(0,34));
            }
            for(String id:objects)count(store,id,counts.getOrDefault(id,0L));
        } finally { Native.unlock(fd); }
    }
    private static void collect(File file,long dev,Set<String> live) throws IOException {
        PlaceholderFiles.Stat s=PlaceholderFiles.backend().lstat(file.getPath());
        if(s==null||s.dev!=dev)return;
        if(s.directory()) {
            for(File child:children(file))if(!child.getName().equals(".v3-inodes"))collect(child,dev,live);
        } else if(s.regular()&&s.sticky()&&s.uid==PlaceholderFiles.backend().uid())live.add(key(s));
    }
}
