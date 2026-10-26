package org.matonos.hostsmoke;

import java.io.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.ForkJoinPool;
import org.matonos.linuxhost.stubgen.*;

/** Host adapter that calls the production OSTree reader and EROFS image writer. */
public final class HostSmokePacker {
    private static String run(File dir, String... args) throws Exception {
        List<String> command=new ArrayList<>();command.add("ostree");command.add("--repo="+dir.getAbsolutePath());
        command.addAll(Arrays.asList(args));
        Process p=new ProcessBuilder(command).redirectErrorStream(true).start();
        byte[] out=p.getInputStream().readAllBytes();
        if(p.waitFor()!=0)throw new IOException(String.join(" ",command)+": "+new String(out,StandardCharsets.UTF_8));
        return new String(out,StandardCharsets.UTF_8).trim();
    }
    private static byte[] read(File file) throws IOException { return Files.readAllBytes(file.toPath()); }
    private static String metadata(File deployment) throws IOException {
        return Files.readString(new File(deployment,"metadata").toPath());
    }
    private static String get(String text,String section,String key) {
        boolean active=false;
        for(String line:text.split("\\R")) {
            line=line.trim();
            if(line.startsWith("[")){active=line.equals("["+section+"]");continue;}
            int equals=line.indexOf('=');if(active&&equals>=0&&line.substring(0,equals).trim().equals(key))return line.substring(equals+1).trim();
        }
        return null;
    }
    private static String deploymentCommit(File dir) throws Exception {
        Path active=new File(dir,"active").toPath();
        if(Files.isSymbolicLink(active))return Files.readSymbolicLink(active).getFileName().toString();
        String value=Files.readString(active).trim();
        if(value.matches("[0-9a-f]{64}"))return value;
        throw new IOException("Cannot resolve deployment active ref: "+active);
    }
    private static File deployment(File installation,String ref) throws Exception {
        String[] parts=ref.split("/");
        File branch=new File(new File(installation,parts[0]+"/"+parts[1]),parts[2]+"/"+parts[3]);
        String commit=deploymentCommit(branch);File result=new File(branch,commit);
        if(!new File(result,"files").isDirectory())throw new IOException("Missing files/ deployment for "+ref);
        return result;
    }
    private static String safeName(String ref) { return ref.replace('/','_'); }
    private static File commitObject(File repo,String commit) { return new File(repo,"objects/"+commit.substring(0,2)+"/"+commit.substring(2)+".commit"); }
    public static void main(String[] a) throws Exception {
        if(a.length!=4)throw new IllegalArgumentException("usage: HostSmokePacker INSTALLATION WORK_DIR MKFS_EROFS APP_REF");
        File installation=new File(a[0]),work=new File(a[1]),mkfs=new File(a[2]);String appRef=a[3];
        File repo=new File(installation,"repo");List<String> refs=new ArrayList<>();
        for(String kind:new String[]{"app","runtime"}) {
            File root=new File(installation,kind);File[] ids=root.listFiles();if(ids==null)continue;
            for(File id:ids){File[] arches=id.listFiles();if(arches==null)continue;for(File arch:arches){File[] branches=arch.listFiles();if(branches==null)continue;for(File branch:branches)if(new File(branch,"active").exists())refs.add(kind+"/"+id.getName()+"/"+arch.getName()+"/"+branch.getName());}}
        }
        Collections.sort(refs);if(!refs.contains(appRef))throw new IOException("Flatpak ref did not install: "+appRef+" installed="+refs);
        Map<String,File> deployments=new LinkedHashMap<>();Map<String,String> commits=new LinkedHashMap<>(),metadata=new LinkedHashMap<>();
        for(String ref:refs){File d=deployment(installation,ref);deployments.put(ref,d);metadata.put(ref,metadata(d));commits.put(ref,run(repo,"rev-parse",ref));}
        LinkedHashSet<String> selected=new LinkedHashSet<>();selected.add(appRef);
        String declaredRuntime=get(metadata.get(appRef),"Application","runtime");
        if(declaredRuntime==null)throw new IOException("App metadata has no runtime: "+appRef);
        if(!declaredRuntime.startsWith("runtime/"))declaredRuntime="runtime/"+declaredRuntime;
        if(!deployments.containsKey(declaredRuntime))throw new IOException("App runtime is not installed: "+declaredRuntime);
        selected.add(declaredRuntime);
        boolean grew;
        do {
            grew=false;
            for(String child:refs) {
                String parent=get(metadata.get(child),"ExtensionOf","ref");
                if(parent==null)continue;
                if(!deployments.containsKey(parent)&&parent.matches("runtime/[^/]+/i386/[^/]+"))
                    parent=parent.replace("/i386/","/x86_64/");
                if(selected.contains(parent)&&selected.add(child))grew=true;
            }
        } while(grew);
        List<String> graphRefs=new ArrayList<>();graphRefs.add(appRef);
        for(String ref:selected)if(!ref.equals(appRef))graphRefs.add(ref);
        File imageRoot=new File(work,"images");Files.createDirectories(imageRoot.toPath());
        for(String ref:graphRefs) {
            File d=deployments.get(ref),source=new File(d,"files");
            File out=new File(imageRoot,safeName(ref));Files.createDirectories(out.toPath());
            String packageName=org.matonos.linux.stub.DeployManifest.packageForPublic(ref);
            File stamp=new File(out,"source-commit");boolean reusable=stamp.isFile()&&Files.readString(stamp.toPath()).trim().equals(commits.get(ref))&&new File(out,"deployment.erofs").isFile()&&new File(out,"deployment.bin").isFile()&&new File(out,"objects.index").isFile();
            String split;long size;
            if(!reusable) {
                File op=new File(work,"staging/"+safeName(ref));Files.createDirectories(op.toPath());File localRepo=new File(op,"repo");run(localRepo,"init","--mode=bare-user");
                String localCommit=OstreePacks.commitDeployed(source,(String... args)->run(localRepo,args));
                OstreePacks packed=OstreePacks.read(localRepo,localCommit,(String... args)->run(localRepo,args),(file,name)->null);
                // Runtime image paths include its branch, exactly as the Android packer does.
                List<ObjectPacks.Entry> entries=packed.entries;
                if(ref.startsWith("runtime/")) {
                    String branch=ref.substring(ref.lastIndexOf('/')+1);List<ObjectPacks.Entry> rooted=new ArrayList<>();
                    for(ObjectPacks.Entry e:entries){ObjectPacks.Entry n=new ObjectPacks.Entry("/"+branch+(e.path.equals("/")?"":e.path),e.checksum,e.kind,e.mode);n.target=e.target;n.xattrs=e.xattrs;n.uid=e.uid;n.gid=e.gid;n.mtime=e.mtime;n.length=e.length;n.source=e.source;rooted.add(n);}entries=rooted;
                }
                ErofsImage.BuildResult built=ErofsImage.build(entries,out,mkfs,Math.max(1,packed.commitTime));split=built.image.split;size=built.image.imageSize;
                ObjectPacks.initial(entries,out,ObjectPacks.DEFAULT_PACK_SIZE);
                ObjectPacks.writeIndex(new File(out,"objects.index"),entries);
                ObjectPacks.writeMetadata(new File(out,"objects.ostree"),packed.metadata);
                Files.write(new File(out,"deployment.bin").toPath(),new DeploymentRecord(ref,commits.get(ref),"flathub",
                    read(new File(d,"metadata")),read(new File(d,"deploy")),read(commitObject(repo,commits.get(ref)))).encode());
                StubGenerator.generateErofsPackUnsigned(out,packageName,new File(out,"deployment.erofs"),1);
                File indexSplit=new File(out,"index_"+safeName(ref)+".apk");
                java.lang.reflect.Method indexWriter=StubGenerator.class.getDeclaredMethod("writeIndexUnsigned",File.class,String.class,File.class,int.class);
                indexWriter.setAccessible(true);indexWriter.invoke(null,indexSplit,packageName,out,1);
                Files.writeString(stamp.toPath(),commits.get(ref)+"\n");
            } else {split=StubGenerator.erofsSplitName(new File(out,"deployment.erofs"));size=new File(out,"deployment.erofs").length();}
            System.out.println("IMAGE\t"+ref+"\t"+split+"\t"+size+"\t"+new File(out,"deployment.erofs").getAbsolutePath()+"\t"+new File(d,"files").getAbsolutePath());
        }
        // The common launcher target planner is the same function used by PackageMounts on device.
        List<String> graph=new ArrayList<>();
        for(String ref:graphRefs){String parent=get(metadata.get(ref),"ExtensionOf","ref");if(parent!=null&&!deployments.containsKey(parent)&&parent.matches("runtime/[^/]+/i386/[^/]+"))parent=parent.replace("/i386/","/x86_64/");if(parent!=null&&!parent.startsWith("app/")&&!parent.startsWith("runtime/"))parent=null;
            if(parent!=null)graph.add(parent+"\t"+ref);}
        String appRuntime=get(metadata.get(appRef),"Application","runtime");if(appRuntime!=null){if(!appRuntime.startsWith("runtime/"))appRuntime="runtime/"+appRuntime;graph.add(appRef+"\t"+appRuntime);}
        // PackageMounts requires the authenticated dependency edges for each declared package.
        List<DeploymentRecord> records=new ArrayList<>();
        for(String ref:graphRefs){if(!ref.equals(appRef)&&!reachable(ref,appRef,graph))continue;File d=deployments.get(ref);List<DeploymentRecord.Dependency> deps=new ArrayList<>();for(String edge:graph){String[] p=edge.split("\\t");if(p[0].equals(ref)&&deployments.containsKey(p[1]))deps.add(new DeploymentRecord.Dependency(p[1],"flathub",mount(p[1])));}
            records.add(new DeploymentRecord(ref,commits.get(ref),"flathub",read(new File(d,"metadata")),read(new File(d,"deploy")),read(commitObject(repo,commits.get(ref))),deps,mount(ref)));}
        Map<String,String> targets=org.matonos.linux.stub.PackageMounts.launchTargets(records);
        for(Map.Entry<String,String> e:targets.entrySet())if(e.getValue()!=null)System.out.println("TARGET\t"+e.getKey()+"\t"+e.getValue());
    }
    private static String mount(String ref){return ref.startsWith("app/")?"/app":"/usr";}
    private static boolean reachable(String ref,String root,List<String> edges){Set<String> seen=new HashSet<>();ArrayDeque<String> q=new ArrayDeque<>();q.add(root);while(!q.isEmpty()){String at=q.remove();if(!seen.add(at))continue;if(at.equals(ref))return true;for(String line:edges){String[] p=line.split("\\t");if(p[0].equals(at))q.add(p[1]);}}return false;}
}
