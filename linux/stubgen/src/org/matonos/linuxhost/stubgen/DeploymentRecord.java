package org.matonos.linuxhost.stubgen;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** Tiny Flatpak deployment scaffolding carried by the signed index split. */
public final class DeploymentRecord {
    public final String ref, commit, remote, mount;
    public final byte[] metadata, deploy, commitObject;
    public static final class Dependency {
        public final String ref,remote,mount;
        public Dependency(String ref,String remote,String mount)throws IOException {
            if(!DependencyPlan.validRef(ref)||mount==null||!mount.matches("/(?:usr|app)(?:/[A-Za-z0-9_.+-]+)*")
                    ||Arrays.asList(mount.split("/")).contains("..")||Arrays.asList(mount.split("/")).contains("."))throw new IOException("Invalid declared dependency");
            RemoteOrigin.parse(remote);this.ref=ref;this.remote=remote;this.mount=mount;
        }
    }
    public final List<Dependency> dependencies;
    public DeploymentRecord(String ref,String commit,String remote,byte[] metadata,byte[] deploy,byte[] commitObject) throws IOException {
        this(ref,commit,remote,metadata,deploy,commitObject,Collections.emptyList());
    }
    public DeploymentRecord(String ref,String commit,String remote,byte[] metadata,byte[] deploy,byte[] commitObject,List<Dependency> dependencies)throws IOException {
        this(ref,commit,remote,metadata,deploy,commitObject,dependencies,ref!=null&&ref.startsWith("app/")?"/app":"/usr");
    }
    public DeploymentRecord(String ref,String commit,String remote,byte[] metadata,byte[] deploy,byte[] commitObject,List<Dependency> dependencies,String mount)throws IOException {
        new Dependency(ref,remote,mount);this.mount=mount;
        if(dependencies==null)throw new IOException("Invalid dependency list");
        Set<String> refs=new HashSet<>();
        for(Dependency dependency:dependencies)if(!refs.add(dependency.ref)||dependency.ref.equals(ref))throw new IOException("Duplicate or self dependency");
        this.dependencies=List.copyOf(dependencies);
        if(ref==null||!ref.matches("(?:app|runtime)/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
                ||Arrays.asList(ref.split("/")).contains("..")||Arrays.asList(ref.split("/")).contains(".")
                ||commit==null||!commit.matches("[0-9a-f]{64}")||remote==null
                ||metadata.length==0||deploy.length==0)
            throw new IOException("Invalid packaged Flatpak deployment");
        try {
            if(!ObjectPacks.hex(java.security.MessageDigest.getInstance("SHA-256").digest(commitObject)).equals(commit))
                throw new IOException("Packaged OSTree commit checksum mismatch");
        } catch(java.security.NoSuchAlgorithmException impossible){throw new IOException(impossible);}
        RemoteOrigin.parse(remote);
        this.commitObject=commitObject;
        this.ref=ref;this.commit=commit;this.remote=remote;this.metadata=metadata;this.deploy=deploy;
    }
    public byte[] encode() throws IOException {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        try(DataOutputStream out=new DataOutputStream(bytes)) {
            out.writeUTF("MTDEP3");out.writeUTF(ref);out.writeUTF(commit);
            byte[] origin=remote.getBytes(StandardCharsets.UTF_8);out.writeInt(origin.length);out.write(origin);out.writeUTF(mount);
            out.writeInt(commitObject.length);out.write(commitObject);
            out.writeInt(metadata.length);out.write(metadata);out.writeInt(deploy.length);out.write(deploy);
            out.writeInt(dependencies.size());
            for(Dependency dependency:dependencies){out.writeUTF(dependency.ref);out.writeUTF(dependency.mount);writeOrigin(out,dependency.remote);}
        }
        return bytes.toByteArray();
    }
    public static DeploymentRecord decode(byte[] bytes) throws IOException {
        try(DataInputStream in=new DataInputStream(new ByteArrayInputStream(bytes))) {
            String version=in.readUTF();
            if(!version.equals("MTDEP3"))throw new IOException("Unknown deployment record");
            String ref=in.readUTF(),commit=in.readUTF(),remote=new String(blob(in),StandardCharsets.UTF_8);
            String mount=in.readUTF();
            byte[] commitObject=blob(in),metadata=blob(in),deploy=blob(in);
            int count=in.readInt();if(count<0)throw new IOException("Invalid dependency count");
            List<Dependency> dependencies=new ArrayList<>();
            for(int i=0;i<count;i++){String depRef=in.readUTF(),depMount=in.readUTF();dependencies.add(new Dependency(depRef,new String(blob(in),StandardCharsets.UTF_8),depMount));}
            if(in.read()!=-1)throw new IOException("Trailing deployment record bytes");
            return new DeploymentRecord(ref,commit,remote,metadata,deploy,commitObject,dependencies,mount);
        }
    }
    private static void writeOrigin(DataOutputStream out,String origin)throws IOException {byte[] bytes=origin.getBytes(StandardCharsets.UTF_8);out.writeInt(bytes.length);out.write(bytes);}
    private static byte[] blob(DataInputStream in) throws IOException {
        int length=in.readInt();if(length<0)throw new IOException("Invalid deployment field size");
        byte[] bytes=new byte[length];in.readFully(bytes);return bytes;
    }
    public static DeploymentRecord installed(String[] names,String[] paths) throws IOException {
        if(names==null||paths==null||names.length!=paths.length)throw new IOException("Missing deployment split");
        DeploymentRecord found=null;
        for(int i=0;i<names.length;i++)if(names[i].matches("index_[0-9a-f]{64}")) {
            if(found!=null)throw new IOException("Multiple deployment splits");
            try(ZipFile zip=new ZipFile(paths[i])) {
                ZipEntry entry=zip.getEntry("assets/matonos/deployment.bin");
                if(entry==null)throw new IOException("Missing deployment record; reinstall app");
                try(InputStream in=zip.getInputStream(entry)) {
                    byte[] bytes=in.readAllBytes();
                    found=decode(bytes);
                }
            }
        }
        if(found==null)throw new IOException("Missing deployment record; reinstall app");return found;
    }
    /** Build disposable metadata only. Payload bytes stay mounted from the APK. */
    public void materialize(File installation,String mount) throws IOException {
        if(mount==null||!mount.matches("/(?:app|usr)(?:/[A-Za-z0-9_.+-]+)*")
                ||Arrays.asList(mount.split("/")).contains("..")||Arrays.asList(mount.split("/")).contains("."))throw new IOException("Invalid deployment mount");
        Path root=installation.toPath();
        Path branch=root.resolve(ref),version=branch.resolve(commit);
        Files.createDirectories(version);
        Files.write(version.resolve("metadata"),metadata);Files.write(version.resolve("deploy"),deploy);
        Files.write(version.resolve(".ref"),new byte[0]);
        // Flatpak opens this entry with RESOLVE_NO_SYMLINKS when it prepares
        // the original runtime. Payload files are exposed by the loader's
        // signed pack mount at mount, so the deployment scaffold only needs
        // the real directory entry here.
        Files.createDirectory(version.resolve("files"));
        link(branch.resolve("active"),Path.of(commit));
        String[] parts=ref.split("/");
        link(root.resolve(parts[0]).resolve(parts[1]).resolve("current"),Path.of(parts[2],parts[3]));
        Path repo=root.resolve("repo");Files.createDirectories(repo.resolve("objects"));
        Path object=repo.resolve("objects").resolve(commit.substring(0,2)).resolve(commit.substring(2)+".commit");
        Files.createDirectories(object.getParent());Files.write(object,commitObject);
        // Flatpak run enumerates installed refs from OSTree's deployed-ref
        // namespace; active/current links alone are enough for `info`, but
        // without this ref the app is reported as not installed.
        Path deployedRef=repo.resolve("refs/heads/deploy").resolve(ref);
        Files.createDirectories(deployedRef.getParent());
        Files.writeString(deployedRef,commit+"\n",StandardCharsets.US_ASCII);
        Files.createDirectories(repo.resolve("refs/remotes"));
        Files.createDirectories(repo.resolve("tmp"));
        Files.write(repo.resolve("config"),"[core]\nrepo_version=1\nmode=bare-user-only\n".getBytes(StandardCharsets.UTF_8));
    }
    private static void link(Path link,Path target) throws IOException {
        Files.deleteIfExists(link);Files.createSymbolicLink(link,target);
    }
}
