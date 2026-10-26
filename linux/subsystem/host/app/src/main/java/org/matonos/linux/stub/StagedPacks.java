package org.matonos.linux.stub;

import android.content.Context;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.function.BooleanSupplier;
import org.json.JSONObject;
import org.matonos.linuxhost.stubgen.OstreePacks;
import org.matonos.linuxhost.stubgen.ObjectPacks;
import org.matonos.erofs.IErofsMounter;

/** Pack production from a private, signature-verified Flatpak installation. */
public final class StagedPacks {
    public static void build(Context context, File installation, JSONObject manifest,
            File output, BooleanSupplier cancelled, android.content.pm.PackageInfo installed,
            java.util.concurrent.ForkJoinPool workPool) throws Exception {
        org.matonos.linux.WorkDirs.require(installation);
        File deployment=DeployManifest.stagedDeployment(installation,manifest);
        File operation=installation.getParentFile();
        File sourceRepo=new File(installation,"repo");
        OstreePacks.Command source=command(context,operation,sourceRepo,cancelled);
        // The real Flatpak repo is bare-user-only. Read its authenticated object
        // identities first; never change its config/mode or remote refs.
        OstreePacks.Xattrs objectXattrs=(file,name)->{
            try { return android.system.Os.getxattr(file.getAbsolutePath(),name); }
            catch(android.system.ErrnoException error) {
                if(error.errno==android.system.OsConstants.ENODATA||error.errno==android.system.OsConstants.ENOENT)return null;
                throw new IOException("Cannot read OSTree object xattr " + name,error);
            }
        };
        List<String> subpaths=new ArrayList<>();org.json.JSONArray requested=manifest.optJSONArray("subpaths");
        if(requested!=null)for(int i=0;i<requested.length();i++)subpaths.add(requested.getString(i));
        OstreePacks original=OstreePacks.read(sourceRepo,manifest.getString("appCommit"),source,objectXattrs,subpaths);
        // The committed tree is the deployment's files/ exactly as deployed,
        // including the stock apply_extra output. A separate bare-user repo keeps
        // that tree without the bare-user-only restrictions of the download repo:
        // committing into the download repo stores executables as 0600.
        File localRepo=new File(operation,"pack-repo");
        OstreePacks.Command local=command(context,operation,localRepo,cancelled);
        local.run("init","--mode=bare-user");
        long deployStarted=System.nanoTime();File deployedFiles=new File(deployment,"files");
        String commit=OstreePacks.commitDeployed(deployedFiles,local);
        Log.i("MatonInstall","phase=deploy ref="+manifest.optString("ref","unknown")+" ms="+
                java.util.concurrent.TimeUnit.NANOSECONDS.toMillis(System.nanoTime()-deployStarted)+" bytes="+InstallTransaction.treeBytes(deployedFiles));
        OstreePacks fresh=OstreePacks.read(localRepo,commit,local,objectXattrs);
        for(org.matonos.linuxhost.stubgen.ObjectPacks.Entry entry:fresh.entries) {
            if(entry.kind.equals("file")||entry.kind.equals("symlink"))try {
                File deployedPath=entry.path.equals("/")?deployedFiles:new File(deployedFiles,entry.path.substring(1));
                entry.mtime=java.nio.file.Files.getLastModifiedTime(deployedPath.toPath(),java.nio.file.LinkOption.NOFOLLOW_LINKS).toMillis()/1000L;
            } catch(Exception ignored) {}
        }
        List<org.matonos.linuxhost.stubgen.ObjectPacks.Entry> payloads=new ArrayList<>(fresh.entries);
        payloads.addAll(original.entries);
        output.mkdirs();
        long imageEpoch=original.commitTime>0?original.commitTime:fresh.commitTime;
        File erofsTool=LoaderCommand.tool(context,"libmatonos-mkfs-erofs.so");
        List<org.matonos.linuxhost.stubgen.ObjectPacks.Entry> imageEntries=fresh.entries;
        if(manifest.getString("appRef").startsWith("runtime/")) {
            String branch=manifest.getString("appRef").substring(manifest.getString("appRef").lastIndexOf('/')+1);
            imageEntries=new ArrayList<>(fresh.entries.size());
            for(org.matonos.linuxhost.stubgen.ObjectPacks.Entry sourceEntry:fresh.entries) {
                org.matonos.linuxhost.stubgen.ObjectPacks.Entry e=new org.matonos.linuxhost.stubgen.ObjectPacks.Entry(
                        "/"+branch+(sourceEntry.path.equals("/")?"":sourceEntry.path),sourceEntry.checksum,sourceEntry.kind,sourceEntry.mode);
                e.target=sourceEntry.target;e.xattrs=sourceEntry.xattrs;e.uid=sourceEntry.uid;e.gid=sourceEntry.gid;
                e.mtime=sourceEntry.mtime;e.length=sourceEntry.length;e.source=sourceEntry.source;imageEntries.add(e);
            }
        }
        IBinder seedToken=seedUnchangedFromMounted(installed,operation,imageEntries,manifest.getString("appRef"));
        long mkfsStarted=System.nanoTime();long imageBytes;
        File returnedImage=new File(installation,"extra-data-image.erofs");
        if(returnedImage.isFile()) {
            List<ExtraImageValidator.Entry> trusted=new ArrayList<>();
            String trustedPrefix=manifest.getString("appRef").startsWith("runtime/")
                    ? "/"+manifest.getString("appRef").substring(manifest.getString("appRef").lastIndexOf('/')+1)
                    : "/app";
            for(ObjectPacks.Entry sourceEntry:original.entries) {
                String path=sourceEntry.path.equals("/")?trustedPrefix:trustedPrefix+sourceEntry.path;
                trusted.add(new ExtraImageValidator.Entry(path,sourceEntry.kind,sourceEntry.mode,
                        sourceEntry.target,sourceEntry.checksum,sourceEntry.source));
            }
            File extracted=new File(installation,"extra-image-validated");
            try {
                ExtraImageValidator.checkImage(
                        LoaderCommand.tool(context,"libmatonos-fsck-erofs.so"),returnedImage,extracted,trusted);
                File extra=new File(extracted,"app/extra"),target=new File(deployedFiles,"extra");
                if(extra.exists()) {
                    delete(target);
                    Files.move(extra.toPath(),target.toPath(),java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                }
                try(InputStream in=org.matonos.linuxhost.stubgen.PlaceholderFiles.open(returnedImage);
                        OutputStream out=new FileOutputStream(new File(output,"deployment.erofs"))) {
                    byte[] buffer=new byte[65536];int count;while((count=in.read(buffer))!=-1)out.write(buffer,0,count);
                }
            } finally {delete(extracted);}
            imageBytes=returnedImage.length();
        } else {
            org.matonos.linuxhost.stubgen.ErofsImage.BuildResult imageResult=
                    org.matonos.linuxhost.stubgen.ErofsImage.build(imageEntries,output,erofsTool,imageEpoch,cancelled,workPool);
            imageBytes=imageResult.image.imageSize;
        }
        if(seedToken!=null)seedToken.pingBinder(); // Keep the mounted-image lease alive through the full write.
        Log.i("MatonInstall","phase=mkfs_total ref="+manifest.optString("ref","unknown")+" ms="+
                java.util.concurrent.TimeUnit.NANOSECONDS.toMillis(System.nanoTime()-mkfsStarted)+" bytes="+imageBytes);
        // The prior release's payloads live in its mounted EROFS image, not in
        // pack_ APK carriers. Reuse matching file sources above while building
        // the new single image; this metadata pack index is regenerated from
        // the newly staged tree for extra-data and update bookkeeping.
        org.matonos.linuxhost.stubgen.ObjectPacks.initial(payloads,output,
                org.matonos.linuxhost.stubgen.ObjectPacks.DEFAULT_PACK_SIZE);
        org.matonos.linuxhost.stubgen.ObjectPacks.writeIndex(new File(output,"objects.index"),fresh.entries);
        org.matonos.linuxhost.stubgen.ObjectPacks.writeMetadata(new File(output,"objects.ostree"),fresh.metadata);
        org.matonos.linuxhost.stubgen.ObjectPacks.writeIndex(new File(output,"original.index"),original.entries);
        original.writeOriginal(new File(output,"original.ostree"));
        org.matonos.linux.WorkDirs.require(installation);
        org.matonos.linux.WorkDirs.require(output);
        record(sourceRepo,deployment,manifest,output);
        try {
            android.util.Log.i("MatonInstall","Temporary operation allocated bytes after packs: "+
                    InstallTransaction.allocatedBytes(operation)+"; available: "+operation.getUsableSpace());
        } catch(Exception error) {
            // Concurrent packers may remove OSTree temp files while this
            // informational walk runs; that must not fail a completed install.
            android.util.Log.w("MatonInstall","Cannot measure temporary operation size",error);
        }
    }

    private static void delete(File file) {
        if(java.nio.file.Files.isSymbolicLink(file.toPath())) { file.delete(); return; }
        if(file.isDirectory()) {File[] children=file.listFiles();if(children!=null)for(File child:children)delete(child);}
        file.delete();
    }

    /** Reuse bytes from the installed image where the OSTree object checksum is unchanged. */
    private static IBinder seedUnchangedFromMounted(android.content.pm.PackageInfo installed,File operation,
            List<ObjectPacks.Entry> entries,String ref) {
        if(installed==null||installed.applicationInfo==null||installed.splitNames==null||installed.applicationInfo.splitSourceDirs==null)return null;
        try {
            String imageApk=null;
            for(int i=0;i<installed.splitNames.length&&i<installed.applicationInfo.splitSourceDirs.length;i++)
                if(installed.splitNames[i].matches("erofs_[0-9a-f]{64}")){imageApk=installed.applicationInfo.splitSourceDirs[i];break;}
            if(imageApk==null)return null;
            Map<String,ObjectPacks.Entry> old=new HashMap<>();
            File index=File.createTempFile("old-image-",".index",new File(operation,"tmp"));
            try(java.util.zip.ZipFile zip=new java.util.zip.ZipFile(installed.applicationInfo.sourceDir)) {
                java.util.zip.ZipEntry item=zip.getEntry("assets/matonos/objects.index");if(item==null)return null;
                try(InputStream in=zip.getInputStream(item);OutputStream out=new FileOutputStream(index)){
                    byte[] buffer=new byte[8192];int n;while((n=in.read(buffer))!=-1)out.write(buffer,0,n);
                }
                for(ObjectPacks.Entry e:ObjectPacks.readIndex(index))old.put(e.path,e);
            } finally {index.delete();}
            IBinder token=new Binder();IBinder binder=(IBinder)Class.forName("android.os.ServiceManager")
                    .getMethod("getService",String.class).invoke(null,"matonos.erofs");
            IErofsMounter service=IErofsMounter.Stub.asInterface(binder);if(service==null)return null;
            String mounted=service.mountSplit(imageApk,"assets/matonos/deployment.erofs",token);
            if(mounted==null||!mounted.startsWith("/mnt/matonos/"))return null;
            String branch=ref.startsWith("runtime/")?ref.substring(ref.lastIndexOf('/')+1):null;
            File root=new File(mounted).getCanonicalFile();int reused=0;
            for(ObjectPacks.Entry e:entries) {
                String objectPath=e.path;
                if(branch!=null&&objectPath.startsWith("/"+branch+"/"))objectPath=objectPath.substring(branch.length()+1);
                ObjectPacks.Entry prior=old.get(objectPath);if(prior==null||!prior.checksum.equals(e.checksum)||!"file".equals(e.kind))continue;
                File candidate=new File(root,e.path.startsWith("/")?e.path.substring(1):e.path).getCanonicalFile();
                if(!candidate.getPath().startsWith(root.getPath()+File.separator)||!candidate.isFile()||candidate.length()!=e.length)continue;
                e.source=candidate;reused++;
            }
            Log.i("MatonInstall","phase=erofs_seed ref="+ref+" reused_files="+reused);
            return token;
        } catch(Exception error) {
            // A missing old mount only costs copy time; staged deployment remains complete.
            Log.w("MatonInstall","Cannot seed unchanged objects from installed EROFS image",error);
            return null;
        }
    }

    private static void record(File sourceRepo,File deployment,JSONObject manifest,File output) throws Exception {
        String ref=manifest.getString("appRef"),commit=manifest.getString("appCommit");
        List<org.matonos.linuxhost.stubgen.DeploymentRecord.Dependency> dependencies=new ArrayList<>();
        org.json.JSONArray declared=manifest.getJSONArray("dependencies");
        for(int i=0;i<declared.length();i++) {
            String dependency=declared.getString(i);JSONObject row=InstallTransaction.row(manifest,dependency);
            dependencies.add(new org.matonos.linuxhost.stubgen.DeploymentRecord.Dependency(dependency,row.getString("origin"),row.getString("mount")));
        }
        org.matonos.linuxhost.stubgen.DeploymentRecord record=new org.matonos.linuxhost.stubgen.DeploymentRecord(
                ref,commit,manifest.getString("origin"),org.matonos.linuxhost.stubgen.PlaceholderFiles.readAllBytes(new File(deployment,"metadata")),
                org.matonos.linuxhost.stubgen.PlaceholderFiles.readAllBytes(new File(deployment,"deploy")),
                org.matonos.linuxhost.stubgen.PlaceholderFiles.readAllBytes(new File(sourceRepo,"objects/"+commit.substring(0,2)+"/"+commit.substring(2)+".commit")),dependencies,manifest.getString("mount"));
        Files.write(new File(output,"deployment.bin").toPath(),record.encode());
    }
    /** Flatpak authenticates downloads. Check repository content before packing. */
    public static void verify(Context context,File installation,BooleanSupplier cancelled) throws Exception {
        command(context,installation.getParentFile(),new File(installation,"repo"),cancelled).run("fsck");
    }

    static OstreePacks.Command command(Context context,File operation,File repo,
            BooleanSupplier cancelled) {
        return (String... args)-> {
            if(cancelled.getAsBoolean())throw new IOException("Pack production cancelled");
            List<String> command=new ArrayList<>();
            command.add(LoaderCommand.loader(context).getAbsolutePath());
            command.add("--parent-pid");command.add(Integer.toString(android.os.Process.myPid()));
            // multicall.c explicitly shifts argv for this selector: argv[0]
            // passed to ostree_cli_main is "ostree", not matonos-flatpak.
            command.add(LoaderCommand.tool(context,LoaderCommand.FLATPAK_TOOL).getAbsolutePath());command.add("ostree");
            command.add("--repo="+repo.getAbsolutePath());Collections.addAll(command,args);
            File error=File.createTempFile("pack-ostree-",".log",new File(operation,"tmp"));
            try {
                ProcessBuilder pb=new ProcessBuilder(command).redirectError(error);
                Map<String,String> env=pb.environment();env.clear();
                String nativeDir=LoaderCommand.nativeLibraryDir(context).getAbsolutePath();
                env.put("PATH",nativeDir+":/system/bin");
                env.put("LD_LIBRARY_PATH",nativeDir+":/system/lib64");
                env.put("LC_ALL","C");env.put("HOME",new File(operation,"home").getAbsolutePath());
                env.put("TMPDIR",new File(operation,"tmp").getAbsolutePath());
                pb.directory(new File(operation,"home"));
                Process child=pb.start();
                try {
                    ByteArrayOutputStream out=new ByteArrayOutputStream();
                    try(InputStream in=child.getInputStream()) {
                        byte[] buffer=new byte[8192];int n;
                        while((n=in.read(buffer))!=-1) {
                            if(cancelled.getAsBoolean())throw new IOException("Pack production cancelled");
                            if(out.size()>Integer.MAX_VALUE-n)throw new IOException("OSTree output exceeds JVM array size");
                            out.write(buffer,0,n);
                        }
                    }
                    if(child.waitFor()!=0)throw new IOException("Pack ostree "+args[0]+" failed: "+
                            new String(Files.readAllBytes(error.toPath()),StandardCharsets.UTF_8));
                    return out.toString("UTF-8");
                } finally {child.destroyForcibly();}
            } finally {error.delete();}
        };
    }
}
