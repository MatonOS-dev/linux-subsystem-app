package org.matonos.linuxhost.stubgen;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** Tiny Flatpak deployment scaffolding carried by the signed index split. */
public final class DeploymentRecord {
    public final String ref, commit, remote;
    public final byte[] metadata, deploy, commitObject;
    public DeploymentRecord(String ref,String commit,String remote,byte[] metadata,byte[] deploy,byte[] commitObject) throws IOException {
        if(ref==null||!ref.matches("(?:app|runtime)/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
                ||Arrays.asList(ref.split("/")).contains("..")||Arrays.asList(ref.split("/")).contains(".")
                ||commit==null||!commit.matches("[0-9a-f]{64}")||!"flathub".equals(remote)
                ||metadata.length==0||metadata.length>128*1024||deploy.length==0||deploy.length>1024*1024)
            throw new IOException("Invalid packaged Flatpak deployment");
        try {
            if(commitObject.length>1024*1024||!ObjectPacks.hex(java.security.MessageDigest.getInstance("SHA-256").digest(commitObject)).equals(commit))
                throw new IOException("Packaged OSTree commit checksum mismatch");
        } catch(java.security.NoSuchAlgorithmException impossible){throw new IOException(impossible);}
        this.commitObject=commitObject;
        this.ref=ref;this.commit=commit;this.remote=remote;this.metadata=metadata;this.deploy=deploy;
    }
    public byte[] encode() throws IOException {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        try(DataOutputStream out=new DataOutputStream(bytes)) {
            out.writeUTF("MTDEP1");out.writeUTF(ref);out.writeUTF(commit);out.writeUTF(remote);
            out.writeInt(commitObject.length);out.write(commitObject);
            out.writeInt(metadata.length);out.write(metadata);out.writeInt(deploy.length);out.write(deploy);
        }
        return bytes.toByteArray();
    }
    public static DeploymentRecord decode(byte[] bytes) throws IOException {
        try(DataInputStream in=new DataInputStream(new ByteArrayInputStream(bytes))) {
            if(!in.readUTF().equals("MTDEP1"))throw new IOException("Unknown deployment record");
            String ref=in.readUTF(),commit=in.readUTF(),remote=in.readUTF();
            byte[] commitObject=blob(in,1024*1024),metadata=blob(in,128*1024),deploy=blob(in,1024*1024);
            if(in.read()!=-1)throw new IOException("Trailing deployment record bytes");
            return new DeploymentRecord(ref,commit,remote,metadata,deploy,commitObject);
        }
    }
    private static byte[] blob(DataInputStream in,int limit) throws IOException {
        int length=in.readInt();if(length<=0||length>limit)throw new IOException("Invalid deployment field size");
        byte[] bytes=new byte[length];in.readFully(bytes);return bytes;
    }
    public static DeploymentRecord installed(String[] names,String[] paths) throws IOException {
        if(names==null||paths==null||names.length!=paths.length)throw new IOException("Missing deployment split");
        DeploymentRecord found=null;
        for(int i=0;i<names.length;i++)if(names[i].matches("index_[0-9a-f]{64}")) {
            if(found!=null)throw new IOException("Multiple deployment splits");
            try(ZipFile zip=new ZipFile(paths[i])) {
                ZipEntry entry=zip.getEntry("assets/matonos/deployment.bin");
                if(entry==null||entry.getSize()>3*1024*1024)throw new IOException("Missing deployment record; reinstall app");
                try(InputStream in=zip.getInputStream(entry)) {
                    byte[] bytes=in.readNBytes(3*1024*1024+1);
                    if(bytes.length>3*1024*1024)throw new IOException("Oversized deployment record");
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
        link(version.resolve("files"),Path.of(mount));
        link(branch.resolve("active"),Path.of(commit));
        String[] parts=ref.split("/");
        link(root.resolve(parts[0]).resolve(parts[1]).resolve("current"),Path.of(parts[2],parts[3]));
        Path repo=root.resolve("repo");Files.createDirectories(repo.resolve("objects"));
        Path object=repo.resolve("objects").resolve(commit.substring(0,2)).resolve(commit.substring(2)+".commit");
        Files.createDirectories(object.getParent());Files.write(object,commitObject);
        Files.createDirectories(repo.resolve("refs/heads"));Files.createDirectories(repo.resolve("refs/remotes"));
        Files.createDirectories(repo.resolve("tmp"));
        Files.write(repo.resolve("config"),"[core]\nrepo_version=1\nmode=bare-user-only\n".getBytes(StandardCharsets.UTF_8));
    }
    private static void link(Path link,Path target) throws IOException {
        Files.deleteIfExists(link);Files.createSymbolicLink(link,target);
    }
}
