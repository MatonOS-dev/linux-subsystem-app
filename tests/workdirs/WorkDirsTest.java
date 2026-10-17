import java.io.*;
import java.nio.file.*;
import org.matonos.linux.WorkDirs;
public class WorkDirsTest {
    interface Checked { void run() throws Exception; }
    static void rejects(Checked action) throws Exception {
        try { action.run(); } catch(IOException expected) { return; }
        throw new AssertionError("Expected loud failure");
    }
    public static void main(String[] args) throws Exception {
        File base=Files.createTempDirectory(Path.of(args[0]),"workdirs-").toFile();
        try {
            android.content.Context context=new android.content.Context(){public File getNoBackupFilesDir(){return new File(base,"no_backup");}};
            File root=WorkDirs.root(context);
            if(!root.equals(new File(base,"no_backup/work").getCanonicalFile()))throw new AssertionError("Wrong root");
            WorkDirs.checkDnsPath(new File("/data/data/org.matonos.linux/no_backup/work"));
            rejects(()->WorkDirs.checkDnsPath(new File("/data/data/"+"p".repeat(223)+"/no_backup/work")));
            String suffix="/dns/forwarder.udp";
            WorkDirs.checkDnsPath(new File("/"+"p".repeat(107-1-suffix.length())));
            rejects(()->WorkDirs.checkDnsPath(new File("/"+"p".repeat(108-1-suffix.length()))));
            File live=new File(root,"flatpak-staging/live"), stale=new File(root,"flatpak-staging/stale");
            Files.createDirectories(live.toPath()); Files.createDirectories(stale.toPath());
            Files.writeString(new File(live,"payload").toPath(),"keep");
            File outside=new File(base,"outside");Files.createDirectories(outside.toPath());
            Files.writeString(new File(outside,"keep").toPath(),"keep");
            Files.createSymbolicLink(new File(root,"link").toPath(),outside.toPath());
            try(AutoCloseable lease=WorkDirs.keep(live)) {
                WorkDirs.sweep(root);
                if(!new File(live,"payload").isFile() || stale.exists() || !new File(outside,"keep").isFile())throw new AssertionError("Unsafe sweep");
            }
            WorkDirs.sweep(root);
            if(live.exists())throw new AssertionError("Released operation retained");
            rejects(()->WorkDirs.require(live));
        } finally { WorkDirs.remove(base); }
        System.out.println("WorkDirs tests passed");
    }
}
