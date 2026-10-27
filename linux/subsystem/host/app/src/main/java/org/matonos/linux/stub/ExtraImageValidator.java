package org.matonos.linux.stub;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.Map;

/** Validates a stub-built image after fsck.erofs has extracted it to a private directory. */
public final class ExtraImageValidator {
    public static final class Entry {
        public final String path, kind, target, sha256;
        public final int mode;
        public final File content;
        public Entry(String path, String kind, int mode, String target, String sha256, File content) {
            this.path=path; this.kind=kind; this.mode=mode; this.target=target; this.sha256=sha256; this.content=content;
        }
    }

    private ExtraImageValidator() { }

    /** Runs the pinned fsck.erofs extractor, then validates the result against the trusted commit entries. */
    public static void checkImage(File fsck,File image,File extracted,Iterable<Entry> committed)throws IOException,InterruptedException {
        if(extracted.exists())throw new IOException("EROFS validation directory already exists");
        if(!extracted.mkdirs())throw new IOException("Cannot create EROFS validation directory");
        Process process=new ProcessBuilder(fsck.getAbsolutePath(),"--extract="+extracted.getAbsolutePath(),
                "--preserve-perms",image.getAbsolutePath())
                .redirectErrorStream(true).start();
        byte[] output;
        try(java.io.InputStream in=process.getInputStream()) { output=in.readAllBytes(); }
        int status=process.waitFor();
        if(status!=0)throw new IOException("fsck.erofs rejected stub image (exit status "+status+
                signalHint(status)+"): "+new String(output,java.nio.charset.StandardCharsets.UTF_8));
        validate(extracted,committed);
    }

    /** Process.waitFor exposes signal deaths as 128+signal on Linux, without a separate signal field. */
    private static String signalHint(int status) {
        int signal=status-128;
        if(signal<1||signal>64)return "";
        String name;
        switch(signal) {
            case 1:name="SIGHUP";break; case 2:name="SIGINT";break; case 3:name="SIGQUIT";break;
            case 4:name="SIGILL";break; case 5:name="SIGTRAP";break; case 6:name="SIGABRT";break;
            case 7:name="SIGBUS";break; case 8:name="SIGFPE";break; case 9:name="SIGKILL";break;
            case 10:name="SIGUSR1";break; case 11:name="SIGSEGV";break; case 12:name="SIGUSR2";break;
            case 13:name="SIGPIPE";break; case 14:name="SIGALRM";break; case 15:name="SIGTERM";break;
            case 16:name="SIGSTKFLT";break; case 17:name="SIGCHLD";break; case 18:name="SIGCONT";break;
            case 19:name="SIGSTOP";break; case 20:name="SIGTSTP";break; case 21:name="SIGTTIN";break;
            case 22:name="SIGTTOU";break; case 23:name="SIGURG";break; case 24:name="SIGXCPU";break;
            case 25:name="SIGXFSZ";break; case 26:name="SIGVTALRM";break; case 27:name="SIGPROF";break;
            case 28:name="SIGWINCH";break; case 29:name="SIGIO";break; case 30:name="SIGPWR";break;
            case 31:name="SIGSYS";break; default:name="unknown";break;
        }
        return "; possible signal "+signal+" ("+name+", if terminated by signal)";
    }

    /** Commit entries are exact outside /app/extra. That subtree is only type/mode checked. */
    public static java.util.List<Entry> flatpakFiles(java.lang.Iterable<Entry> commitEntries,String targetRoot)
            throws IOException {
        if(targetRoot==null||!targetRoot.startsWith("/")||targetRoot.equals("/")
                ||targetRoot.endsWith("/")||targetRoot.contains("//"))
            throw new IOException("Invalid Flatpak deployment target");
        java.util.List<Entry> result=new java.util.ArrayList<>();
        boolean foundFilesRoot=false;
        for(Entry entry:commitEntries) {
            if(entry.path.equals("/files")) {
                foundFilesRoot=true;
                result.add(new Entry(targetRoot,entry.kind,entry.mode,entry.target,entry.sha256,entry.content));
            } else if(entry.path.startsWith("/files/")) {
                result.add(new Entry(targetRoot+entry.path.substring("/files".length()),entry.kind,
                        entry.mode,entry.target,entry.sha256,entry.content));
            }
        }
        if(!foundFilesRoot)throw new IOException("Flatpak commit has no /files deployment tree");
        return result;
    }

    public static void validate(File root, Iterable<Entry> committed) throws IOException {
        Path base=root.toPath().toRealPath(LinkOption.NOFOLLOW_LINKS);
        Map<String,Entry> expected=new HashMap<>();
        for(Entry e:committed) {
            String p=canonical(e.path);
            if(expected.put(p,e)!=null)throw new IOException("Duplicate committed image path: "+p);
        }
        expected.keySet().removeIf(p->p.equals("/app/extra")||p.startsWith("/app/extra/"));
        walk(base,base,expected);
        for(String path:expected.keySet())if(!Files.exists(base.resolve(path.substring(1)),LinkOption.NOFOLLOW_LINKS))
            throw new IOException("EROFS image omits committed path: "+path);
    }

    private static void walk(Path base,Path dir,Map<String,Entry> expected)throws IOException {
        try(java.nio.file.DirectoryStream<Path> children=Files.newDirectoryStream(dir)) {
            for(Path item:children) {
                String relative="/"+base.relativize(item).toString().replace(File.separatorChar,'/');
                boolean extra=relative.equals("/app/extra")||relative.startsWith("/app/extra/");
                if(extra) {
                    if(relative.equals("/app/extra")&&!Files.isDirectory(item,LinkOption.NOFOLLOW_LINKS))
                        throw new IOException("/app/extra must be a directory");
                    validateExtra(base.resolve("app/extra"),item); continue;
                }
                Entry entry=expected.remove(relative);
                if(entry==null)throw new IOException("EROFS image contains uncommitted path: "+relative);
                boolean symlink=Files.isSymbolicLink(item),directory=Files.isDirectory(item,LinkOption.NOFOLLOW_LINKS),file=Files.isRegularFile(item,LinkOption.NOFOLLOW_LINKS);
                int actualMode=mode(item), expectedMode=entry.mode&07777;
                if(actualMode!=expectedMode)throw new IOException(String.format(java.util.Locale.ROOT,
                        "EROFS mode differs from commit: %s (expected %04o, actual %04o)",relative,expectedMode,actualMode));
                if(entry.kind.equals("dir")) {
                    if(!directory)throw new IOException("EROFS type differs from commit: "+relative);
                    walk(base,item,expected);
                } else if(entry.kind.equals("symlink")) {
                    if(!symlink||!Files.readSymbolicLink(item).toString().equals(entry.target))throw new IOException("EROFS symlink differs from commit: "+relative);
                } else if(entry.kind.equals("file")) {
                    if(!file||!sha256(item.toFile()).equals(entry.sha256))throw new IOException("EROFS file differs from commit: "+relative);
                } else throw new IOException("Invalid committed entry kind: "+entry.kind);
            }
        }
    }

    private static void validateExtra(Path extraRoot,Path item)throws IOException {
        if(Files.isSymbolicLink(item)) {
            Path target=Files.readSymbolicLink(item);
            if(target.isAbsolute()||!item.getParent().resolve(target).normalize().startsWith(extraRoot))
                throw new IOException("Extra-data symlink escapes /app/extra: "+item);
            return;
        }
        if(Files.isDirectory(item,LinkOption.NOFOLLOW_LINKS)) {
            if((mode(item)&06000)!=0)throw new IOException("Setuid or setgid extra-data directory");
            try(java.nio.file.DirectoryStream<Path> children=Files.newDirectoryStream(item)) { for(Path child:children)validateExtra(extraRoot,child); }
        } else if(Files.isRegularFile(item,LinkOption.NOFOLLOW_LINKS)) {
            if((mode(item)&06000)!=0)throw new IOException("Setuid or setgid extra-data file");
        } else throw new IOException("Special file is not allowed under /app/extra: "+item);
    }

    private static String canonical(String value)throws IOException {
        if(value==null||!value.startsWith("/")||value.equals("/")||value.contains("\\")||value.contains("//"))throw new IOException("Invalid committed path");
        for(String p:value.substring(1).split("/"))if(p.isEmpty()||p.equals(".")||p.equals(".."))throw new IOException("Invalid committed path");
        return value;
    }
    private static int mode(Path path)throws IOException {
        try { return ((Number)Files.getAttribute(path,"unix:mode",LinkOption.NOFOLLOW_LINKS)).intValue()&07777; }
        catch(UnsupportedOperationException error){throw new IOException("EROFS validation requires POSIX mode support",error);}
    }
    private static String sha256(File file)throws IOException {
        try {java.security.MessageDigest d=java.security.MessageDigest.getInstance("SHA-256");try(java.io.InputStream in=Files.newInputStream(file.toPath())){byte[] b=new byte[65536];int n;while((n=in.read(b))!=-1)d.update(b,0,n);}StringBuilder s=new StringBuilder();for(byte b:d.digest())s.append(String.format(java.util.Locale.ROOT,"%02x",b&255));return s.toString();}
        catch(java.security.NoSuchAlgorithmException e){throw new AssertionError(e);}
    }
}
