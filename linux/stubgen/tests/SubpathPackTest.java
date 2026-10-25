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
    private static OstreePacks.Xattrs xattrs() { return (file,name)->null; }
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
        Files.writeString(new File(tree,"en/nested/deep.txt").toPath(),"English nested\n");
        Files.writeString(new File(tree,"de/bonjour.txt").toPath(),"bonjour\n");
        Files.writeString(new File(tree,"de/nested/other.txt").toPath(),"German nested\n");
        OstreePacks.Command src=cli(source);src.run("init","--mode=bare-user");
        String commit=src.run("commit","--branch=locale","--parent=none","--no-xattrs",
                "--canonical-permissions","--subject=locale subpath fixture","--tree=dir="+tree.getAbsolutePath());
        OstreePacks all=OstreePacks.read(source,commit,src,xattrs());
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
                "/en/hello.txt","/en/nested/deep.txt"));
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
        Files.writeString(fakeMkfs.toPath(),"#!/bin/sh\ncat \"${10}\" > \"$9\"\n");check(fakeMkfs.setExecutable(true),"Cannot make fake mkfs executable");
        ErofsLayers.Index index=ErofsLayers.build(packed.entries,layerOut,fakeMkfs,0,null,Collections.emptyMap());
        check(!index.layers.isEmpty(),"No layers produced");
        ErofsLayers.Index decoded=ErofsLayers.read(Files.readAllBytes(new File(layerOut,"layers.index").toPath()));
        check(decoded.records.size()==packed.entries.size(),"Layer index lost deployment entries");
        Path extracted=new File(root,"extracted").toPath();Files.createDirectories(extracted);
        for(ErofsLayers.Layer layer:decoded.layers)run("tar","-xf",new File(layerOut,"pack-"+layer.sha256+".erofs").toString(),"-C",extracted.toString());
        compareTree(checkout.toPath(),extracted);
        System.out.println("PASS /en-only OSTree pull, scoped indexes and layer extraction match Flatpak-style checkout; /de objects were absent");
    }
}
