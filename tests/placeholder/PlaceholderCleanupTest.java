import org.matonos.linuxhost.stubgen.PlaceholderCleanup;
import java.nio.*;
import java.nio.file.*;
import java.util.*;

public final class PlaceholderCleanupTest {
    static final String ID="i-0123456789abcdef0123456789abcdef";
    static void check(boolean ok,String what) { if(!ok)throw new AssertionError(what); }
    static String key(Path p)throws Exception {
        return String.format(Locale.ROOT,"p-%016x-%016x",Files.getAttribute(p,"unix:dev"),Files.getAttribute(p,"unix:ino"));
    }
    static void count(Path store,long n)throws Exception {
        Files.write(store.resolve(ID+".count"),ByteBuffer.allocate(8).order(ByteOrder.nativeOrder()).putLong(n).array());
    }
    static long count(Path store)throws Exception {
        return ByteBuffer.wrap(Files.readAllBytes(store.resolve(ID+".count"))).order(ByteOrder.nativeOrder()).getLong();
    }
    static Path placeholder(Path root,Path store,String name)throws Exception {
        Path p=root.resolve(name);Files.createDirectories(p.getParent());Files.createFile(p);
        Files.setAttribute(p,"unix:mode",01000);
        Files.write(store.resolve(key(p)),(ID+"\0").getBytes(java.nio.charset.StandardCharsets.US_ASCII));
        return p;
    }
    public static void main(String[] args)throws Exception {
        Path root=Files.createTempDirectory("link-gc-");
        try {
            PlaceholderCleanup.sweep(root.toFile());
            check(!Files.exists(root.resolve(".v3-inodes")),"no-store sweep creates nothing");
            Path store=Files.createDirectory(root.resolve(".v3-inodes"));Files.setAttribute(store,"unix:mode",0700);
            Files.write(store.resolve(ID),new byte[]{42});count(store,2);
            Path a=placeholder(root,store,"tree/a"),b=placeholder(root,store,"keep/b");String ak=key(a);
            // util-linux flock uses the exact syscall used by compat-proxy.
            Process holder=new ProcessBuilder("flock","-x",store.resolve("lock").toString(),
                    "sh","-c","echo locked; read release").start();
            check(new java.io.BufferedReader(new java.io.InputStreamReader(holder.getInputStream())).readLine().equals("locked"),"external flock acquired");
            java.util.concurrent.CompletableFuture<Void> deleted=new java.util.concurrent.CompletableFuture<>();
            Thread worker=new Thread(() -> {
                try {PlaceholderCleanup.deleteTree(root.resolve("tree").toFile());deleted.complete(null);}
                catch(Throwable e){deleted.completeExceptionally(e);}
            });
            worker.start();
            try {
                try {deleted.get(150,java.util.concurrent.TimeUnit.MILLISECONDS);throw new AssertionError("cleanup bypassed flock");}
                catch(java.util.concurrent.TimeoutException expected) { }
                check(Files.exists(a)&&count(store)==2,"flock blocks deletion and count mutation");
            } finally {
                holder.getOutputStream().write("release\n".getBytes());holder.getOutputStream().close();
                holder.waitFor();
            }
            deleted.get(10,java.util.concurrent.TimeUnit.SECONDS);
            check(count(store)==1&&Files.exists(store.resolve(ID)),"2 -> 1 keeps object");
            check(!Files.exists(store.resolve(ak)),"deleted placeholder index removed");
            PlaceholderCleanup.deleteTree(root.resolve("keep").toFile());
            check(!Files.exists(store.resolve(ID))&&!Files.exists(store.resolve(ID+".count")),"last link frees object");
            Files.write(store.resolve(ID),new byte[]{42});count(store,99);
            a=placeholder(root,store,"live");b=placeholder(root,store,"stale");String bk=key(b);Files.delete(b);
            String dead="i-ffffffffffffffffffffffffffffffff";Files.write(store.resolve(dead),new byte[]{0});
            Files.write(store.resolve(dead+".count"),new byte[8]);
            Path orphanCount=store.resolve("i-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.count");
            Files.write(orphanCount,new byte[8]);
            Files.write(store.resolve("intent"),new byte[]{1});
            PlaceholderCleanup.sweep(root.toFile());check(count(store)==99&&Files.exists(store.resolve(bk)),"journal leaves store alone");
            boolean refused=false;
            try {PlaceholderCleanup.deleteTree(a.toFile());}catch(java.io.IOException e){refused=true;}
            check(refused&&Files.exists(a),"delete refuses pending journal");
            Files.delete(store.resolve("intent"));
            Path outside=Files.createTempDirectory("link-gc-outside-");
            Files.createSymbolicLink(root.resolve("outside"),outside);
            PlaceholderCleanup.sweep(root.toFile());
            check(count(store)==1&&Files.exists(store.resolve(key(a))),"sweep recounts live placeholder");
            check(!Files.exists(orphanCount),"sweep removes orphan sidecar");
            check(!Files.exists(store.resolve(bk))&&!Files.exists(store.resolve(dead))&&!Files.exists(store.resolve(dead+".count")),"sweep frees stale entries and objects");
            Files.delete(outside);
            PlaceholderCleanup.deleteTree(a.toFile());
            System.out.println("PlaceholderCleanupTest passed");
        } finally {
            try(java.util.stream.Stream<Path> paths=Files.walk(root)) {
                for(Path p:(Iterable<Path>)paths.sorted(Comparator.reverseOrder())::iterator)Files.deleteIfExists(p);
            }
        }
    }
}
