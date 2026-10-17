package org.matonos.linuxhost.stubgen;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.*;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Bare-user reader. Binary GVariant framing for metadata; stock CLI for tree traversal.
 * No checkout, filename listing parser, or Android/native dependency. */
public final class OstreePacks {
    public interface Command { String run(String... arguments) throws Exception; }
    public interface Xattrs { byte[] get(File file, String name) throws IOException; }
    public final List<ObjectPacks.Entry> entries = new ArrayList<>();
    public final Map<String,byte[]> metadata = new HashMap<>();
    private final Map<String,byte[]> original = new TreeMap<>();
    private final File repo;
    private final Command cli;
    private final Xattrs xattrs;
    private String commit;
    private OstreePacks(File repo, Command cli, Xattrs xattrs) { this.repo=repo; this.cli=cli; this.xattrs=xattrs; }

    public static OstreePacks read(File repo, String revision, Command cli, Xattrs xattrs) throws Exception {
        OstreePacks result=new OstreePacks(repo,cli,xattrs);
        result.commit=cli.run("rev-parse",revision).trim();
        checksum(result.commit);
        byte[][] c=tuple(result.object(result.commit,"commit"),
                new int[]{8,1,1,1,1,8,1,1},new int[]{-1,-1,-1,-1,-1,8,-1,-1});
        result.visit(hex(c[6]),hex(c[7]),"/",new HashSet<>(),0);
        return result;
    }
    /** Private ref is local-only, scoped to the operation's own staging repo. */
    public static String commitDeployed(File files, Command cli) throws Exception {
        if(!files.isDirectory() || Files.isSymbolicLink(files.toPath())) throw new IOException("Missing deployed files tree");
        String value=cli.run("commit","--branch=matonos/private/deployed-files","--parent=none",
                "--owner-uid=0","--owner-gid=0","--no-xattrs","--canonical-permissions",
                "--subject=MatonOS local deployed files","--tree=dir="+files.getAbsolutePath()).trim();
        checksum(value); return value;
    }
    public void write(File output) throws Exception {
        ObjectPacks.initial(entries,output,ObjectPacks.DEFAULT_PACK_SIZE);
        ObjectPacks.writeIndex(new File(output,"objects.index"),entries);
        ObjectPacks.writeMetadata(new File(output,"objects.ostree"),metadata);
    }
    /** v3.5: original metadata only; deployed-files metadata stays independent. */
    public void writeOriginal(File output) throws Exception {
        try(java.util.zip.ZipOutputStream zip=new java.util.zip.ZipOutputStream(new FileOutputStream(output))) {
            for(Map.Entry<String,byte[]> e:original.entrySet()) {
                java.util.zip.ZipEntry entry=new java.util.zip.ZipEntry(e.getKey());entry.setTime(0);
                zip.putNextEntry(entry);zip.write(e.getValue());zip.closeEntry();
            }
        }
    }
    /** Restore only authenticated metadata. fsck must succeed before enabling deltas. */
    public static void restoreOriginal(InputStream input,File repo,String checksum,byte[] commitObject,Command cli) throws Exception {
        try(java.util.zip.ZipInputStream zip=new java.util.zip.ZipInputStream(input)) {
            java.util.zip.ZipEntry entry;
            while((entry=zip.getNextEntry())!=null) {
                String name=entry.getName();
                if(!name.matches("[0-9a-f]{64}\\.(?:dirtree|dirmeta)"))throw new IOException("Invalid original metadata name");
                byte[] raw=zip.readAllBytes();
                if(!ObjectPacks.hex(MessageDigest.getInstance("SHA-256").digest(raw)).equals(name.substring(0,64)))
                    throw new IOException("Original metadata checksum mismatch");
                File target=new File(repo,"objects/"+name.substring(0,2)+"/"+name.substring(2));
                Files.createDirectories(target.toPath().getParent());Files.write(target.toPath(),raw);
            }
        }
        File commit=new File(repo,"objects/"+checksum.substring(0,2)+"/"+checksum.substring(2)+".commit");
        Files.createDirectories(commit.toPath().getParent());Files.write(commit.toPath(),commitObject);
        if(new File(repo,"state/"+checksum+".commitpartial").exists()){Files.deleteIfExists(commit.toPath());throw new IOException("Original commit is partial");}
        try {cli.run("fsck","--all","--delete");}
        catch(Exception error){Files.deleteIfExists(commit.toPath());throw error;}
        cli.run("refs","--create=matonos/private/update-base/"+checksum,checksum);
    }
    private static void checksum(String s) throws IOException {
        if(!s.matches("[0-9a-f]{64}"))throw new IOException("Invalid OSTree checksum");
    }
    private File path(String sum,String type) throws IOException {
        checksum(sum); return new File(repo,"objects/"+sum.substring(0,2)+"/"+sum.substring(2)+"."+type);
    }
    private byte[] object(String sum,String type) throws Exception {
        byte[] raw=PlaceholderFiles.readAllBytes(path(sum,type));
        if(!ObjectPacks.hex(MessageDigest.getInstance("SHA-256").digest(raw)).equals(sum))
            throw new IOException("OSTree metadata checksum mismatch");
        metadata.put(sum,raw);
        if(!type.equals("commit"))original.put(sum+"."+type,raw);
        return raw;
    }
    private void visit(String tree,String meta,String prefix,Set<String> ancestors,int depth) throws Exception {
        if(depth>256 || !ancestors.add(tree))throw new IOException("Recursive OSTree tree");
        byte[][] m=tuple(object(meta,"dirmeta"),new int[]{4,4,4,1},new int[]{4,4,4,-1});
        ObjectPacks.Entry dir=new ObjectPacks.Entry(prefix,tree,"dir",be(m[2]));
        dir.uid=be(m[0]);dir.gid=be(m[1]);entries.add(dir);
        byte[][] t=tuple(object(tree,"dirtree"),new int[]{1,1},new int[]{-1,-1});
        for(byte[] raw:array(t[1])) {
            byte[][] d=tuple(raw,new int[]{1,1,1},new int[]{-1,-1,-1});
            visit(hex(d[1]),hex(d[2]),child(prefix,d[0]),ancestors,depth+1);
        }
        for(byte[] raw:array(t[0])) {
            byte[][] f=tuple(raw,new int[]{1,1},new int[]{-1,-1});
            String name=child(prefix,f[0]),sum=hex(f[1]);
            File payload=path(sum,"file");
            byte[] rawMeta=xattrs.get(payload,"user.ostreemeta");
            int uid,gid,mode;byte[] packedXattrs;
            if(rawMeta!=null) {
                byte[][] attributes=tuple(rawMeta,new int[]{4,4,4,1},new int[]{4,4,4,-1});
                for(byte[] attribute:array(attributes[3]))tuple(attribute,new int[]{1,1},new int[]{-1,-1});
                uid=be(attributes[0]);gid=be(attributes[1]);mode=be(attributes[2]);packedXattrs=attributes[3];
            } else {
                // bare-user-only objects deliberately have no user.ostreemeta.
                // Preserve support for those repositories; normal bare-user
                // repositories stay entirely in-process.
                String text=cli.run("ls","--dironly","--checksum",commit,"--",name);
                Matcher head=Pattern.compile("^([-l])([0-7]+) +([0-9]+) +([0-9]+) +[0-9]+ +([0-9a-f]{64})").matcher(text);
                if(!head.find()||!sum.equals(head.group(5)))throw new IOException("Unexpected ostree file header");
                mode=Integer.parseInt(head.group(2),8)|(head.group(1).equals("l")?0120000:0100000);
                uid=(int)Long.parseLong(head.group(3));gid=(int)Long.parseLong(head.group(4));packedXattrs=new byte[0];
            }
            int fileType=mode&0170000;
            String kind=fileType==0120000?"symlink":fileType==0100000?"file":null;
            if(kind==null)throw new IOException("Unsupported OSTree file mode for "+sum);
            ObjectPacks.Entry e=new ObjectPacks.Entry(name,sum,kind,mode);
            e.uid=uid;e.gid=gid;e.xattrs=packedXattrs;
            if(e.kind.equals("file"))e.source=payload;
            else if(Files.isSymbolicLink(payload.toPath())) {
                e.target=Files.readSymbolicLink(payload.toPath()).toString();
            } else {
                byte[] target=PlaceholderFiles.readAllBytes(payload);
                if(target.length==0 || target[target.length-1]!=0)throw new IOException("Invalid bare-user symlink");
                e.target=new String(target,0,target.length-1,StandardCharsets.UTF_8);
            }
            entries.add(e);
        }
        ancestors.remove(tree);
    }
    private static String child(String parent,byte[] bytes) throws IOException {
        if(bytes.length<2 || bytes[bytes.length-1]!=0)throw new IOException("Invalid OSTree name");
        String name=new String(bytes,0,bytes.length-1,StandardCharsets.UTF_8);
        if(name.equals(".")||name.equals("..")||name.indexOf('/')>=0||name.indexOf('\0')>=0)
            throw new IOException("Unsafe OSTree name");
        return (parent.equals("/")?parent:parent+"/")+name;
    }
    private static String hex(byte[] b) throws IOException {String s=ObjectPacks.hex(b);checksum(s);return s;}
    private static int be(byte[] b) throws IOException {
        if(b.length!=4)throw new IOException("Invalid uint32");
        return (b[0]&255)<<24|(b[1]&255)<<16|(b[2]&255)<<8|(b[3]&255);
    }
    private static int width(int n) {return n<=255?1:n<=65535?2:4;}
    private static int offset(byte[] b,int pos,int width) throws IOException {
        if(pos<0||pos+width>b.length)throw new IOException("Truncated GVariant offset");
        long n=0;for(int i=0;i<width;i++)n|=(long)(b[pos+i]&255)<<(8*i);
        if(n>Integer.MAX_VALUE)throw new IOException("Oversized GVariant");return (int)n;
    }
    private static byte[] slice(byte[] b,int start,int end) throws IOException {
        if(start<0||end<start||end>b.length)throw new IOException("Invalid GVariant framing");
        return Arrays.copyOfRange(b,start,end);
    }
    private static byte[][] tuple(byte[] b,int[] alignment,int[] fixed) throws IOException {
        int w=width(b.length),framing=b.length,pos=0;
        byte[][] result=new byte[fixed.length][];
        for(int i=0;i<fixed.length;i++) {
            pos=(pos+alignment[i]-1)&-alignment[i];int end;
            if(fixed[i]>=0)end=pos+fixed[i];
            else if(i==fixed.length-1)end=framing;
            else {framing-=w;end=offset(b,framing,w);}
            if(end>framing)throw new IOException("Overlapping GVariant framing");
            result[i]=slice(b,pos,end);pos=end;
        }
        if(pos!=framing)throw new IOException("Trailing GVariant bytes");return result;
    }
    private static List<byte[]> array(byte[] b) throws IOException {
        List<byte[]> out=new ArrayList<>();if(b.length==0)return out;
        int w=width(b.length),end=offset(b,b.length-w,w);
        if(end>b.length-w || (b.length-end)%w!=0)throw new IOException("Invalid array framing");
        int start=0;for(int p=end;p<b.length;p+=w){int next=offset(b,p,w);if(next>end)throw new IOException("Invalid array element");out.add(slice(b,start,next));start=next;}
        return out;
    }
    private static byte[] framed(List<byte[]> elements) throws IOException {
        int n=0;for(byte[] b:elements)n=Math.addExact(n,b.length);
        int w=1;while(width(Math.addExact(n,elements.size()*w))>w)w=width(n+elements.size()*w);
        ByteArrayOutputStream out=new ByteArrayOutputStream();for(byte[] b:elements)out.write(b);
        int end=0;for(byte[] b:elements){end+=b.length;for(int i=0;i<w;i++)out.write(end>>>(8*i));}return out.toByteArray();
    }
}
