import java.io.*;
import java.nio.file.*;
import java.util.*;
import org.matonos.linuxhost.stubgen.*;

/** Exercises packaging against a real OSTree subpath-only pull. */
public final class SubpathPackTest {
    private static String run(String... args) throws Exception {
        Process p=new ProcessBuilder(args).redirectError(ProcessBuilder.Redirect.INHERIT).start();
        String out=new String(p.getInputStream().readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
        if(p.waitFor()!=0)throw new IOException("Command failed: "+Arrays.toString(args));return out.trim();
    }
    private static OstreePacks.Command cli(File repo) {
        return args->{List<String> command=new ArrayList<>(Arrays.asList("ostree","--repo="+repo));
            Collections.addAll(command,args);return run(command.toArray(new String[0]));};
    }
    private static OstreePacks.Xattrs xattrs() { return (file,name)->{
        String key=name.startsWith("user.")?name.substring(5):name;
        java.nio.file.attribute.UserDefinedFileAttributeView view=Files.getFileAttributeView(
                file.toPath(),java.nio.file.attribute.UserDefinedFileAttributeView.class);
        if(view==null||!view.list().contains(key))return null;
        java.nio.ByteBuffer value=java.nio.ByteBuffer.allocate(view.size(key));view.read(key,value);
        return Arrays.copyOf(value.array(),value.position());
    }; }
    private static void check(boolean value,String message) { if(!value)throw new AssertionError(message); }
    private static void compareTree(Path expected,Path actual) throws Exception {
        try(java.util.stream.Stream<Path> paths=Files.walk(expected)) {
            for(Path source:(Iterable<Path>)paths::iterator) {
                Path target=actual.resolve(expected.relativize(source));
                check(Files.exists(target,LinkOption.NOFOLLOW_LINKS),"Missing extracted path "+target);
                if(Files.isDirectory(source,LinkOption.NOFOLLOW_LINKS))check(Files.isDirectory(target,LinkOption.NOFOLLOW_LINKS),"Not a directory: "+target);
                else if(Files.isSymbolicLink(source))check(Files.readSymbolicLink(source).equals(Files.readSymbolicLink(target)),"Symlink differs: "+target);
                else check(Arrays.equals(Files.readAllBytes(source),Files.readAllBytes(target)),"File differs: "+target);
            }
        }
        try(java.util.stream.Stream<Path> paths=Files.walk(actual)) {
            for(Path path:(Iterable<Path>)paths::iterator)check(Files.exists(expected.resolve(actual.relativize(path)),LinkOption.NOFOLLOW_LINKS),"Unexpected extracted path "+path);
        }
    }
    public static void main(String[] args) throws Exception {
        File root=new File(args[0]);root.mkdirs();File source=new File(root,"source"),partial=new File(root,"partial");
        File tree=new File(root,"tree");
        Files.createDirectories(new File(tree,"en/nested").toPath());Files.createDirectories(new File(tree,"de/nested").toPath());
        Files.writeString(new File(tree,"en/hello.txt").toPath(),"hello in English\n");
        Files.writeString(new File(tree,"en/manifest-base-1.json").toPath(),"{}\n");
        Files.setPosixFilePermissions(new File(tree,"en/hello.txt").toPath(),
                java.nio.file.attribute.PosixFilePermissions.fromString("rwxr-xr-x"));
        Files.setPosixFilePermissions(new File(tree,"en/manifest-base-1.json").toPath(),
                java.nio.file.attribute.PosixFilePermissions.fromString("rw-r--r--"));
        Files.writeString(new File(tree,"en/nested/deep.txt").toPath(),"English nested\n");
        Files.writeString(new File(tree,"de/bonjour.txt").toPath(),"bonjour\n");
        Files.writeString(new File(tree,"de/nested/other.txt").toPath(),"German nested\n");
        OstreePacks.Command src=cli(source);src.run("init","--mode=bare-user");
        String commit=src.run("commit","--branch=locale","--parent=none",
                "--canonical-permissions","--subject=locale subpath fixture","--tree=dir="+tree.getAbsolutePath());
        OstreePacks all=OstreePacks.read(source,commit,src,xattrs());
        ObjectPacks.Entry executable=all.entries.stream().filter(e->e.path.equals("/en/hello.txt")).findFirst().orElseThrow();
        ObjectPacks.Entry manifest=all.entries.stream().filter(e->e.path.equals("/en/manifest-base-1.json")).findFirst().orElseThrow();
        check(executable.mode==0100755,"real OSTree file mode was misread for executable: "+Integer.toOctalString(executable.mode));
        check(manifest.mode==0100644,"real OSTree file mode was misread for manifest: "+Integer.toOctalString(manifest.mode));
        OstreePacks.Command dst=cli(partial);dst.run("init","--mode=bare-user-only");
        dst.run("remote","add","--no-gpg-verify","test","file://"+source.getAbsolutePath());
        dst.run("pull","--subpath=/en","test","locale");
        String pulled=dst.run("rev-parse","test:locale");check(commit.equals(pulled),"Subpath pull changed commit identity");
        for(ObjectPacks.Entry entry:all.entries)if(entry.path.startsWith("/de/")&&entry.kind.equals("file"))
            check(!new File(partial,"objects/"+entry.checksum.substring(0,2)+"/"+entry.checksum.substring(2)+".file").exists(),
                    "Subpath pull unexpectedly fetched /de payload "+entry.path);
        OstreePacks original=OstreePacks.read(partial,commit,dst,xattrs(),Collections.singletonList("/en"));
        Set<String> actual=new TreeSet<>();for(ObjectPacks.Entry entry:original.entries)actual.add(entry.path);
        Set<String> expected=new TreeSet<>(Arrays.asList("/","/en","/en/nested",
                "/en/hello.txt","/en/manifest-base-1.json","/en/nested/deep.txt"));
        check(actual.equals(expected),"Scoped source entries differ: "+actual);
        File originalIndex=new File(root,"original.index");ObjectPacks.writeIndex(originalIndex,original.entries);
        byte[] encodedIndex=Files.readAllBytes(originalIndex.toPath());
        check(new String(encodedIndex,0,8,java.nio.charset.StandardCharsets.US_ASCII).equals("MTPKIDX1")
                &&java.nio.ByteBuffer.wrap(encodedIndex,8,4).order(java.nio.ByteOrder.LITTLE_ENDIAN).getInt()==expected.size(),
                "Original index contains unrelated entries");
        File objectPacks=new File(root,"object-packs");objectPacks.mkdirs();
        ObjectPacks.initial(original.entries,objectPacks,ObjectPacks.DEFAULT_PACK_SIZE);
        File objectsIndex=new File(objectPacks,"objects.index");ObjectPacks.writeIndex(objectsIndex,original.entries);
        Set<String> indexed=new TreeSet<>();for(ObjectPacks.Entry entry:ObjectPacks.readIndex(objectsIndex))indexed.add(entry.path);
        check(indexed.equals(expected),"Object index contains unrelated entries: "+indexed);
        File originalObjects=new File(root,"original.objects");ObjectPacks.writeMetadata(originalObjects,original.metadata);
        File originalArchive=new File(root,"original.ostree");original.writeOriginal(originalArchive);
        try(java.util.zip.ZipFile archive=new java.util.zip.ZipFile(originalArchive)) {
            check(archive.size()==4,"Original metadata archive includes unselected directory metadata");
            for(java.util.Enumeration<? extends java.util.zip.ZipEntry> entries=archive.entries();entries.hasMoreElements();) {
                String name=entries.nextElement().getName();
                check(original.metadata.containsKey(name.substring(0,64)),"Original metadata archive has an unknown object");
            }
        }

        File checkout=new File(root,"checkout");dst.run("checkout","--user-mode","--subpath=/en",commit,checkout.toString());
        File localRepo=new File(root,"local");OstreePacks.Command local=cli(localRepo);local.run("init","--mode=bare-user");
        String deployed=OstreePacks.commitDeployed(checkout,local);
        OstreePacks packed=OstreePacks.read(localRepo,deployed,local,xattrs());
        File layerOut=new File(root,"layers");layerOut.mkdirs();File fakeMkfs=new File(root,"fake-mkfs-erofs");
        Files.writeString(fakeMkfs.toPath(),"#!/bin/sh\ncat \"${11}\" > \"${10}\"\n");check(fakeMkfs.setExecutable(true),"Cannot make fake mkfs executable");
        ErofsImage.BuildResult index=ErofsImage.build(packed.entries,layerOut,fakeMkfs,0);
        check(index.image!=null,"Package must produce one EROFS image");
        check(new File(layerOut,"deployment.erofs").isFile()&&!new File(layerOut,"layers.index").exists(),"Single image output must not carry a layer index");
        Path extracted=new File(root,"extracted").toPath();Files.createDirectories(extracted);
        run("tar","-xf",new File(layerOut,"deployment.erofs").toString(),"-C",extracted.toString());
        compareTree(checkout.toPath(),extracted);
        List<ObjectPacks.Entry> duplicateEntries=new ArrayList<>(packed.entries);ObjectPacks.Entry fileEntry=null;
        for(ObjectPacks.Entry e:packed.entries)if(e.kind.equals("file")){fileEntry=e;break;}
        check(fileEntry!=null,"Subpath fixture must contain a regular file");
        ObjectPacks.Entry duplicate=new ObjectPacks.Entry("/zz-checksum-hardlink",fileEntry.checksum,"file",fileEntry.mode);
        duplicate.source=fileEntry.source;duplicate.length=fileEntry.length;duplicate.mtime=fileEntry.mtime;
        duplicate.uid=fileEntry.uid;duplicate.gid=fileEntry.gid;duplicate.xattrs=fileEntry.xattrs;duplicateEntries.add(duplicate);
        File dedupeOut=new File(root,"dedupe-image");dedupeOut.mkdirs();
        ErofsImage.build(duplicateEntries,dedupeOut,fakeMkfs,0);
        String listing=run("tar","-tvf",new File(dedupeOut,"deployment.erofs").toString());
        check(listing.contains("zz-checksum-hardlink link to "),"Identical OSTree checksum must be emitted as a tar hardlink");
        System.out.println("PASS /en-only OSTree pull, scoped indexes, one-image tar, checksum hardlinks and extraction match Flatpak-style checkout; /de objects were absent");
    }
}
