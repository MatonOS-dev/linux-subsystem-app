package org.matonos.linux.stub;

import android.content.Context;
import android.util.Log;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;

/** Persistent catalogue installation; operation-private payload installations use the same CLI. */
public final class StoreFlatpak {
    private final Context context;
    public org.matonos.linux.store.SourceOperations.Operation cancellation;
    public final File root,installation;
    public StoreFlatpak(Context context,File root) throws Exception {
        this.context=context;this.root=root;installation=new File(root,"installation");
        for(String dir:Arrays.asList("installation","home","cache","tmp","run"))Files.createDirectories(new File(root,dir).toPath());
    }
    public String cli(String... arguments) throws Exception {return command(LoaderCommand.FLATPAK_CLI,arguments);}
    public String helper(String... arguments) throws Exception {return command("libmatonos-flatpak-plan.so",arguments);}
    private String command(String tool,String... arguments) throws Exception {
        boolean flatpak=LoaderCommand.FLATPAK_CLI.equals(tool);
        boolean verbose=flatpak&&verboseEnabled();
        List<String> command=new ArrayList<>();String dns=LoaderCommand.resolverAddress();
        File resolv=LoaderCommand.writeResolv(new File(root,"run/resolv.conf"),dns);
        command.add(LoaderCommand.loader(context).getAbsolutePath());command.add("--parent-pid");command.add(Integer.toString(android.os.Process.myPid()));
        LoaderCommand.addDns(command,dns);LoaderCommand.addResolvMaps(context,command,resolv);
        command.add(LoaderCommand.tool(context,tool).getAbsolutePath());
        if(verbose){command.add("-vv");command.add("--ostree-verbose");}
        Collections.addAll(command,arguments);
        File errors=File.createTempFile("store-flatpak-",".log",new File(root,"tmp"));
        ProcessBuilder builder=new ProcessBuilder(command).directory(new File(root,"home")).redirectError(errors);
        Map<String,String> env=builder.environment();env.clear();String nativeDir=LoaderCommand.nativeLibraryDir(context).getAbsolutePath();
        env.put("PATH",nativeDir+":/system/bin");env.put("LD_LIBRARY_PATH",nativeDir+":/system/lib64");env.put("LC_ALL","C");
        env.put("HOME",new File(root,"home").getAbsolutePath());env.put("TMPDIR",new File(root,"tmp").getAbsolutePath());env.put("XDG_RUNTIME_DIR",new File(root,"run").getAbsolutePath());
        env.put("XDG_DATA_HOME",new File(root,"home/.local/share").getAbsolutePath());env.put("XDG_CONFIG_HOME",new File(root,"home/.config").getAbsolutePath());env.put("XDG_CACHE_HOME",new File(root,"cache").getAbsolutePath());
        for(String key:Arrays.asList("FLATPAK_SYSTEM_DIR","FLATPAK_USER_DIR"))env.put(key,installation.getAbsolutePath());
        env.put("FLATPAK_SYSTEM_CACHE_DIR",new File(root,"cache").getAbsolutePath());
        // CURL_CA_BUNDLE names a single PEM file for curl's CLI. The static
        // libcurl uses its build-time Android Conscrypt CApath instead.
        for(String key:Arrays.asList("SSL_CERT_DIR","G_TLS_CA_PATH"))env.put(key,"/apex/com.android.conscrypt/cacerts");
        if(verbose)env.put("OSTREE_DEBUG_HTTP","1");
        Process process=null;
        try {
            if(cancellation!=null)cancellation.check();
            process=builder.start();if(cancellation!=null)cancellation.process(process);byte[] output;
            try(InputStream stream=process.getInputStream()){output=stream.readAllBytes();}
            int status=process.waitFor();String diagnostics=new String(org.matonos.linuxhost.stubgen.PlaceholderFiles.readAllBytes(errors),StandardCharsets.UTF_8);
            if(!diagnostics.isBlank())Log.i("FlatpakSource","command="+Arrays.toString(arguments)+" pid="+process.toString()+" stderr="+diagnostics);
            if(status!=0)throw new IOException("Flatpak "+Arrays.toString(arguments)+" pid="+process.toString()+" exit="+status+": "+diagnostics);
            return new String(output,StandardCharsets.UTF_8);
        }catch(Exception error){Log.e("FlatpakSource","command="+Arrays.toString(arguments)+" failed",error);throw error;}
        finally {if(cancellation!=null)cancellation.release(process);if(process!=null)process.destroy();if(!errors.delete())Log.e("FlatpakSource","Cannot delete diagnostics "+errors);}
    }
    /** Read the public Android system property without depending on hidden APIs. */
    private boolean verboseEnabled() {
        Process process=null;
        try {
            process=new ProcessBuilder("/system/bin/getprop","debug.matonos.flatpak.verbose").start();
            byte[] value;
            try(InputStream stream=process.getInputStream()){value=stream.readAllBytes();}
            int status=process.waitFor();
            if(status!=0){Log.e("FlatpakSource","getprop debug.matonos.flatpak.verbose exited "+status);return false;}
            return "1".equals(new String(value,StandardCharsets.UTF_8).trim());
        } catch(Exception error) {
            Log.e("FlatpakSource","Could not read debug.matonos.flatpak.verbose",error);return false;
        } finally {if(process!=null)process.destroy();}
    }
    /** Export keys along with URLs; bare remote-add URL would lose signature verification. */
    public synchronized void copyRemotesTo(StoreFlatpak target) throws Exception {
        for(String line:cli("remotes","--system","--columns=name,url").split("\n")) {
            if(line.isBlank())continue;
            String[] fields=line.split("\t",-1);if(fields.length!=2)throw new IOException("Malformed remote row: "+line);
            String encoded=fields[0].equals("flathub")?"flathub":origin(fields[0],fields[1]);
            File descriptor=new File(target.root,fields[0]+".flatpakrepo");
            if(encoded.equals("flathub")) {
                try(InputStream input=context.getAssets().open("flatpak/remotes.d/flathub.flatpakrepo")) {
                    try(OutputStream out=Files.newOutputStream(descriptor.toPath())){input.transferTo(out);}
                }
            }else Files.write(descriptor.toPath(),org.matonos.linuxhost.stubgen.RemoteOrigin.parse(encoded).flatpakrepo().getBytes(StandardCharsets.UTF_8));
            target.cli("remote-add","--system",fields[0],descriptor.getAbsolutePath());
        }
    }
    public synchronized String origin(String name) throws Exception {
        if(name.equals("flathub"))return "flathub";
        if(!name.matches("[A-Za-z0-9_][A-Za-z0-9_.-]*"))throw new IOException("Invalid remote name: "+name);
        for(String line:cli("remotes","--system","--columns=name,url").split("\n")) {
            String[] fields=line.split("\t",-1);
            if(fields.length==2&&fields[0].equals(name))return origin(name,fields[1]);
        }
        throw new IOException("Remote disappeared: "+name);
    }
    private String origin(String name,String url) throws Exception {
        if(!name.matches("[A-Za-z0-9_][A-Za-z0-9_.-]*"))throw new IOException("Invalid remote name: "+name);
        byte[] key=org.matonos.linuxhost.stubgen.PlaceholderFiles.readAllBytes(new File(installation,"repo/"+name+".trustedkeys.gpg"));
        return new org.matonos.linuxhost.stubgen.RemoteOrigin(name,url,Base64.getEncoder().encodeToString(key)).encode();
    }
    /** Reconstruct only signed deployment metadata; payloads stay in their Android packages. */
    public Set<String> installedRefs(boolean updates) throws Exception {return snapshotRefs(updates?"--updates":"--installed");}
    /** Installed refs stock flatpak uninstall --unused would remove. */
    public Set<String> unusedRefs() throws Exception {return snapshotRefs("--unused");}
    /** Carrier packages whose every installed ref is unused; a package still holding a needed branch stays. */
    public Set<String> unusedPackages() throws Exception {
        Set<String> unused=unusedRefs();java.util.Map<String,Boolean> packages=new java.util.LinkedHashMap<>();
        InstalledPackages installed=new InstalledPackages(context);
        for(String pkg:installed.empty)packages.put(pkg,true);
        for(String ref:installed.records.keySet()) {
            String pkg=DeployManifest.packageForPublic(ref);packages.put(pkg,packages.getOrDefault(pkg,true)&&unused.contains(ref));
        }
        Set<String> result=new LinkedHashSet<>();for(java.util.Map.Entry<String,Boolean> entry:packages.entrySet())if(entry.getValue())result.add(entry.getKey());
        return result;
    }
    private Set<String> snapshotRefs(String query) throws Exception {
        File work=Files.createTempDirectory(org.matonos.linux.WorkDirs.directory(context,"store-queries").toPath(),"snapshot-").toFile();
        try(AutoCloseable lease=org.matonos.linux.WorkDirs.keep(work)) {
            StoreFlatpak snapshot=new StoreFlatpak(context,work);
            InstalledPackages installed=new InstalledPackages(context);
            for(org.matonos.linuxhost.stubgen.DeploymentRecord record:installed.records.values()) {
                record.materialize(snapshot.installation,record.mount);
                DeployManifest.Metadata identity=DeployManifest.parse(record.deploy);
                if(!identity.subpaths.isEmpty()) {
                    org.json.JSONObject manifest=new org.json.JSONObject().put("appRef",record.ref).put("appCommit",record.commit)
                            .put("subpaths",new org.json.JSONArray(identity.subpaths));
                    File expected=DeployManifest.stagedDeploymentPath(snapshot.installation,manifest);
                    Files.move(new File(snapshot.installation,record.ref+"/"+record.commit).toPath(),expected.toPath());
                    java.nio.file.Path active=new File(snapshot.installation,record.ref+"/active").toPath();
                    Files.delete(active);Files.createSymbolicLink(active,expected.toPath().getFileName());
                }
            }
            if(installed.records.isEmpty())return Collections.emptySet();
            copyRemotesTo(snapshot);
            Set<String> refs=new LinkedHashSet<>();
            for(String line:snapshot.helper(query,snapshot.installation.getAbsolutePath()).split("\n")) {
                if(line.isBlank())continue;String[] fields=line.split("\t",-1);
                if(fields.length!=2||!installed.records.containsKey(fields[0]))throw new IOException("Unexpected Flatpak installed ref: "+line);
                refs.add(fields[0]);
            }
            return refs;
        }finally {try{org.matonos.linux.WorkDirs.remove(work);}catch(Exception error){Log.e("FlatpakSource","Cannot remove installed snapshot "+work,error);}}
    }
    public void preset() throws Exception {
        File descriptor=new File(root,"flathub.flatpakrepo");
        try(InputStream in=context.getAssets().open("flatpak/remotes.d/flathub.flatpakrepo")){try(OutputStream out=Files.newOutputStream(descriptor.toPath())){in.transferTo(out);}}
        cli("remote-add","--system","--if-not-exists","flathub",descriptor.getAbsolutePath());
    }
}
