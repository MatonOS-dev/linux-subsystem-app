package org.matonos.linux.stub;

import android.content.Context;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.function.BooleanSupplier;
import org.json.JSONObject;
import org.matonos.linuxhost.stubgen.OstreePacks;

/** Pack production from a private, signature-verified Flatpak installation. */
public final class StagedPacks {
    public static void build(Context context, File installation, JSONObject manifest,
            File output, BooleanSupplier cancelled, android.content.pm.PackageInfo installed) throws Exception {
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
        OstreePacks original=OstreePacks.read(sourceRepo,manifest.getString("appCommit"),source,objectXattrs);
        // The committed tree is the deployment's files/ exactly as deployed,
        // including the raw extra downloads apply_extra has not consumed (the
        // installer defers those to the stub). A separate bare-user repo keeps
        // that tree without the bare-user-only restrictions of the download repo.
        File localRepo=new File(operation,"pack-repo");
        OstreePacks.Command local=command(context,operation,localRepo,cancelled);
        local.run("init","--mode=bare-user");
        String commit=OstreePacks.commitDeployed(new File(deployment,"files"),local);
        OstreePacks fresh=OstreePacks.read(localRepo,commit,local,objectXattrs);
        List<org.matonos.linuxhost.stubgen.ObjectPacks.Entry> payloads=new ArrayList<>(fresh.entries);
        payloads.addAll(original.entries);
        output.mkdirs();
        if(installed==null || installed.getLongVersionCode()!=org.matonos.linuxhost.stubgen.StubGenerator.STUB_VERSION_CODE) {
            org.matonos.linuxhost.stubgen.ObjectPacks.initial(payloads,output,org.matonos.linuxhost.stubgen.ObjectPacks.DEFAULT_PACK_SIZE);
        } else {
            List<String> args=org.matonos.linuxhost.stubgen.PackLaunch.arguments(new File(installed.applicationInfo.sourceDir),
                    installed.splitNames,installed.applicationInfo.splitSourceDirs,new File(output.getParentFile(),"installed"));
            org.matonos.linuxhost.stubgen.ObjectPacks.update(payloads,
                    org.matonos.linuxhost.stubgen.ObjectPacks.readIndex(new File(args.get(2))),
                    new File(args.get(3)),output,0.5);
        }
        org.matonos.linuxhost.stubgen.ObjectPacks.writeIndex(new File(output,"objects.index"),fresh.entries);
        org.matonos.linuxhost.stubgen.ObjectPacks.writeMetadata(new File(output,"objects.ostree"),fresh.metadata);
        org.matonos.linuxhost.stubgen.ObjectPacks.writeIndex(new File(output,"original.index"),original.entries);
        original.writeOriginal(new File(output,"original.ostree"));
        org.matonos.linux.WorkDirs.require(installation);
        org.matonos.linux.WorkDirs.require(output);
        record(sourceRepo,deployment,manifest,output);
        android.util.Log.i("MatonInstall","Temporary operation allocated bytes after packs: "+
                InstallTransaction.allocatedBytes(operation)+"; available: "+operation.getUsableSpace());
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
                            if(out.size()>16*1024*1024-n)throw new IOException("OSTree output too large");
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
