package org.matonos.linux.stub;

import android.content.Context;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.function.BooleanSupplier;
import org.json.JSONObject;
import org.matonos.linuxhost.stubgen.OstreePacks;

/** Local pack production, before linuxd takes ownership of staging. */
public final class StagedPacks {
    public static void build(Context context, File installation, JSONObject manifest,
            File output, BooleanSupplier cancelled, android.content.pm.PackageInfo installed) throws Exception {
        File deployment=DeployManifest.stagedDeployment(installation,manifest);
        File operation=installation.getParentFile();
        File sourceRepo=new File(installation,"repo");
        OstreePacks.Command source=command(context,operation,sourceRepo,cancelled);
        // The real Flatpak repo is bare-user-only. Read its authenticated object
        // identities first; never change its config/mode or remote refs.
        OstreePacks.read(sourceRepo,manifest.getString("appCommit"),source);
        // A separate bare-user repo preserves post-apply_extra metadata without
        // the bare-user-only mode restrictions imposed on the download repo.
        File localRepo=new File(operation,"pack-repo");
        OstreePacks.Command local=command(context,operation,localRepo,cancelled);
        local.run("init","--mode=bare-user");
        String commit=OstreePacks.commitDeployed(new File(deployment,"files"),local);
        OstreePacks fresh=OstreePacks.read(localRepo,commit,local);
        if(installed==null || installed.getLongVersionCode()!=org.matonos.linuxhost.stubgen.StubGenerator.STUB_VERSION_CODE) {
            fresh.write(output);
        } else {
            List<String> args=org.matonos.linuxhost.stubgen.PackLaunch.arguments(new File(installed.applicationInfo.sourceDir),
                    installed.splitNames,installed.applicationInfo.splitSourceDirs,new File(output.getParentFile(),"installed"));
            org.matonos.linuxhost.stubgen.ObjectPacks.update(fresh.entries,
                    org.matonos.linuxhost.stubgen.ObjectPacks.readIndex(new File(args.get(2))),
                    new File(args.get(3)),output,0.5);
            org.matonos.linuxhost.stubgen.ObjectPacks.writeIndex(new File(output,"objects.index"),fresh.entries);
            org.matonos.linuxhost.stubgen.ObjectPacks.writeMetadata(new File(output,"objects.ostree"),fresh.metadata);
        }
    }

    private static OstreePacks.Command command(Context context,File operation,File repo,
            BooleanSupplier cancelled) {
        return (String... args)-> {
            if(cancelled.getAsBoolean())throw new IOException("Pack production cancelled");
            List<String> command=new ArrayList<>();
            command.add(LoaderCommand.loader(context).getAbsolutePath());
            command.add("--parent-pid");command.add(Integer.toString(android.os.Process.myPid()));
            // multicall.c explicitly shifts argv for this selector: argv[0]
            // passed to ostree_cli_main is "ostree", not matonos-flatpak.
            command.add(LoaderCommand.FLATPAK_BIN+"/matonos-flatpak");command.add("ostree");
            command.add("--repo="+repo.getAbsolutePath());Collections.addAll(command,args);
            File error=File.createTempFile("pack-ostree-",".log",new File(operation,"tmp"));
            try {
                ProcessBuilder pb=new ProcessBuilder(command).redirectError(error);
                Map<String,String> env=pb.environment();env.clear();
                env.put("PATH",LoaderCommand.FLATPAK_BIN+":/system/bin");
                env.put("LD_LIBRARY_PATH","/apex/com.matonos.flatpak/lib64:/system/lib64");
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
