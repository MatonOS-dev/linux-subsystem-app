import java.io.*;
import java.nio.file.*;
import java.util.*;
import org.matonos.linuxhost.stubgen.*;

public final class ErofsLayersHarness {
    private static ObjectPacks.Entry dir(String path) {
        ObjectPacks.Entry e=new ObjectPacks.Entry(path,"", "dir",040755);e.uid=0;e.gid=0;return e;
    }
    private static ObjectPacks.Entry file(String path,File source,String id) throws Exception {
        ObjectPacks.Entry e=new ObjectPacks.Entry(path,id.repeat(64),"file",0100644);e.uid=0;e.gid=0;e.source=source;return e;
    }
    private static ObjectPacks.Entry link(String path,String target) {
        ObjectPacks.Entry e=new ObjectPacks.Entry(path,"a".repeat(64),"symlink",0120777);e.uid=0;e.gid=0;e.target=target;return e;
    }
    private static void makeA(File src,File out,File mkfs)throws Exception {
        Files.createDirectories(new File(src,"usr/bin").toPath());Files.createDirectories(new File(src,"usr/share/doc/deleted-dir").toPath());
        Files.writeString(new File(src,"usr/bin/keep").toPath(),"k".repeat(1024));Files.writeString(new File(src,"usr/share/doc/change").toPath(),"old");
        Files.writeString(new File(src,"usr/share/doc/gone").toPath(),"delete");Files.writeString(new File(src,"usr/share/doc/file-link").toPath(),"file");
        Files.createSymbolicLink(new File(src,"usr/share/doc/link").toPath(),Path.of("../bin/keep"));
        Files.createSymbolicLink(new File(src,"usr/share/doc/remove-link").toPath(),Path.of("../bin/keep"));
        Files.writeString(new File(src,"usr/share/doc/deleted-dir/child").toPath(),"gone");
        List<ObjectPacks.Entry> rows=new ArrayList<>(Arrays.asList(dir("/"),dir("/usr"),dir("/usr/bin"),dir("/usr/share"),dir("/usr/share/doc"),dir("/usr/share/doc/deleted-dir")));
        rows.add(file("/usr/bin/keep",new File(src,"usr/bin/keep"),"1"));rows.add(file("/usr/share/doc/change",new File(src,"usr/share/doc/change"),"2"));
        rows.add(file("/usr/share/doc/gone",new File(src,"usr/share/doc/gone"),"3"));rows.add(file("/usr/share/doc/file-link",new File(src,"usr/share/doc/file-link"),"4"));
        rows.add(link("/usr/share/doc/link","../bin/keep"));rows.add(link("/usr/share/doc/remove-link","../bin/keep"));
        rows.add(file("/usr/share/doc/deleted-dir/child",new File(src,"usr/share/doc/deleted-dir/child"),"5"));
        ErofsLayers.build(rows,out,mkfs,1700000000L,null,Collections.emptyMap());
    }
    private static void makeB(File src,File old,File out,File mkfs)throws Exception {
        ErofsLayers.Index prior=ErofsLayers.read(Files.readAllBytes(new File(old,"layers.index").toPath()));
        Map<String,File> images=new HashMap<>();for(ErofsLayers.Layer l:prior.layers)images.put(l.sha256,new File(old,"pack-"+l.sha256+".erofs"));
        Files.writeString(new File(src,"usr/share/doc/change").toPath(),"new");Files.writeString(new File(src,"usr/share/doc/new").toPath(),"added");
        Files.createSymbolicLink(new File(src,"usr/share/doc/added-link").toPath(),Path.of("../bin/keep"));
        List<ObjectPacks.Entry> rows=new ArrayList<>(Arrays.asList(dir("/"),dir("/usr"),dir("/usr/bin"),dir("/usr/share"),dir("/usr/share/doc")));
        rows.add(file("/usr/bin/keep",new File(src,"usr/bin/keep"),"1"));rows.add(file("/usr/share/doc/change",new File(src,"usr/share/doc/change"),"6"));
        rows.add(file("/usr/share/doc/new",new File(src,"usr/share/doc/new"),"7"));rows.add(link("/usr/share/doc/file-link","../bin/keep"));
        rows.add(link("/usr/share/doc/link","../bin/changed"));rows.add(link("/usr/share/doc/added-link","../bin/keep"));
        ErofsLayers.build(rows,out,mkfs,1700000000L,Files.readAllBytes(new File(old,"layers.index").toPath()),images);
    }
    private static void checkDeterministic(File first,File second)throws Exception {
        if(!Arrays.equals(Files.readAllBytes(new File(first,"layers.index").toPath()),Files.readAllBytes(new File(second,"layers.index").toPath())))
            throw new AssertionError("identical Java layer input changed layers.index bytes");
        ErofsLayers.Index index=ErofsLayers.read(Files.readAllBytes(new File(first,"layers.index").toPath()));
        for(ErofsLayers.Layer layer:index.layers) {
            String name="pack-"+layer.sha256+".erofs";
            if(!Arrays.equals(Files.readAllBytes(new File(first,name).toPath()),Files.readAllBytes(new File(second,name).toPath())))
                throw new AssertionError("identical Java layer input changed image bytes: "+name);
        }
    }
    public static void main(String[] args)throws Exception {
        File root=new File(args[0]),mkfs=new File(args[1]);File srcA=new File(root,"src"),a=new File(root,"A"),repeat=new File(root,"A-repeat"),b=new File(root,"B");
        makeA(srcA,a,mkfs);makeA(new File(root,"src-repeat"),repeat,mkfs);checkDeterministic(a,repeat);makeB(srcA,a,b,mkfs);
        ErofsLayers.Index one=ErofsLayers.read(Files.readAllBytes(new File(a,"layers.index").toPath()));
        ErofsLayers.Index two=ErofsLayers.read(Files.readAllBytes(new File(b,"layers.index").toPath()));
        Set<String> old=new HashSet<>(),next=new HashSet<>();for(ErofsLayers.Layer l:one.layers)old.add(l.sha256);for(ErofsLayers.Layer l:two.layers)next.add(l.sha256);
        System.out.println("layers A="+one.layers.size()+" B="+two.layers.size()+" hashes="+old+" -> "+next);
        Set<String> added=new HashSet<>(next);added.removeAll(old);if(added.size()!=1)throw new AssertionError("expected one new Java EROFS layer: "+added);
        if(Collections.disjoint(old,next))throw new AssertionError("unchanged Java layer was not reused: old="+old+" next="+next);
        Map<String,ErofsLayers.Record> paths=new HashMap<>();for(ErofsLayers.Record r:two.records)paths.put(r.path,r);
        if(paths.containsKey("/usr/share/doc/remove-link"))throw new AssertionError("removed symlink remains indexed");
        if(!"../bin/changed".equals(paths.get("/usr/share/doc/link").target))throw new AssertionError("changed symlink target missing");
        if(!paths.containsKey("/usr/share/doc/added-link"))throw new AssertionError("added symlink missing");
        for(String path:Arrays.asList("/usr/share/doc/file-link","/usr/share/doc/link","/usr/share/doc/added-link"))
            if(paths.get(path).layer==0xffffffff)throw new AssertionError("symlink has no layer: "+path);
        System.out.println("PASS Java EROFS layer writer A->B; A="+one.layers.size()+" layers, B="+two.layers.size()+" layers, new="+added.size());
    }
}
