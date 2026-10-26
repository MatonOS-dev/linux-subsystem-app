package org.matonos.linux.stub;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import java.io.DataInputStream;
import java.io.File;
import java.io.IOException;
import java.util.List;
import java.nio.file.FileVisitResult;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import org.matonos.linuxhost.stubgen.PlaceholderFiles;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.matonos.linux.WorkDirs;

/** Calls stock Flatpak deployment in its isolated-process service and imports only its tarred extra tree. */
final class IsolatedFlatpakInstall {
    static final long TIMEOUT_MS=15*60*1000L;
    private IsolatedFlatpakInstall() { }

    static void deploy(Context context,File installation,String remote,String ref,String commit,
            List<String> subpaths,File deployment) throws Exception {
        WorkDirs.require(installation);
        for(String name:new String[]{"home","tmp","run","cache"})
            java.nio.file.Files.createDirectories(new File(installation,name).toPath());
        java.nio.file.Files.createDirectories(new File(installation,"home/.local/share").toPath());
        java.nio.file.Files.createDirectories(new File(installation,"home/.config").toPath());
        File repoClone=new File(installation,"isolated-repo");
        if(repoClone.exists())remove(repoClone);
        cloneRepo(new File(installation,"repo").toPath(),repoClone.toPath());
        ParcelFileDescriptor[] archive=ParcelFileDescriptor.createPipe();
        ParcelFileDescriptor[] pipe=ParcelFileDescriptor.createPipe();
        CountDownLatch connected=new CountDownLatch(1);
        AtomicReference<IIsolatedFlatpakInstall> remoteService=new AtomicReference<>();
        ServiceConnection connection=new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name,IBinder binder) {
                remoteService.set(IIsolatedFlatpakInstall.Stub.asInterface(binder));connected.countDown();
            }
            @Override public void onServiceDisconnected(ComponentName name) { remoteService.set(null); }
        };
        if(!context.bindService(new Intent(context,IsolatedFlatpakInstallService.class),connection,Context.BIND_AUTO_CREATE)) {
            archive[0].close();archive[1].close();pipe[0].close();pipe[1].close();
            remove(repoClone);
            throw new IOException("Could not bind Flatpak isolated deploy service");
        }
        try(ParcelFileDescriptor archiveRead=archive[0];
                ParcelFileDescriptor.AutoCloseOutputStream archiveOut=new ParcelFileDescriptor.AutoCloseOutputStream(archive[1]);
                ParcelFileDescriptor write=pipe[1];
                ParcelFileDescriptor.AutoCloseInputStream stream=new ParcelFileDescriptor.AutoCloseInputStream(pipe[0])) {
            if(!connected.await(30,TimeUnit.SECONDS)||remoteService.get()==null)
                throw new IOException("Flatpak isolated deploy service did not connect");
            remoteService.get().deploy(archiveRead,write,remote,ref,commit,subpaths.toArray(new String[0]));
            archiveRead.close();
            TarStream.writeDeployment(installation.toPath(),repoClone.toPath(),archiveOut);
            archiveOut.close();
            write.close();
            DataInputStream response=new DataInputStream(stream);
            int status=response.read();
            if(status==2) {
                int length=response.readInt();if(length<0||length>1024*1024)throw new IOException("Invalid isolated deploy error");
                byte[] message=new byte[length];response.readFully(message);throw new IOException(new String(message,java.nio.charset.StandardCharsets.UTF_8));
            }
            if(status!=0)throw new IOException("Unexpected isolated deploy status: "+status);
            File files=new File(deployment,"files");
            File transfer=new File(installation.getParentFile(),"isolated-extra-"+java.util.UUID.randomUUID());
            if(!transfer.mkdirs())throw new IOException("Cannot create isolated extra staging directory");
            try {
                TarStream.extractExtra(response,transfer.toPath());
                File received=new File(transfer,"extra"),target=new File(files,"extra");
                if(!files.isDirectory())throw new IOException("Flatpak deployment has no files directory");
                remove(target);
                if(received.exists()&&!received.renameTo(target))throw new IOException("Cannot move validated extra-data tree into the deployed package");
            } finally {remove(transfer);}
        } finally {
            remove(repoClone);
            try{context.unbindService(connection);}catch(IllegalArgumentException ignored){}
        }
    }

    /** Materialize a normal OSTree view: isolated UID cannot use the subsystem's private hardlink store. */
    private static void cloneRepo(Path source,Path destination) throws IOException {
        java.nio.file.Files.createDirectory(destination);
        java.nio.file.Files.walkFileTree(source,new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult preVisitDirectory(Path dir,BasicFileAttributes attrs)throws IOException {
                Path rel=source.relativize(dir);
                if(rel.toString().equals(".v3-inodes"))return FileVisitResult.SKIP_SUBTREE;
                Path target=destination.resolve(rel);
                java.nio.file.Files.createDirectories(target);
                try {android.system.Os.chmod(target.toString(),0777);}catch(android.system.ErrnoException e){throw new IOException(e);}
                return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult visitFile(Path file,BasicFileAttributes attrs)throws IOException {
                Path rel=source.relativize(file);String name=rel.toString();
                if(name.equals(".v3-store-root"))return FileVisitResult.CONTINUE;
                Path target=destination.resolve(rel);java.nio.file.Files.createDirectories(target.getParent());
                if(java.nio.file.Files.isSymbolicLink(file))java.nio.file.Files.createSymbolicLink(target,java.nio.file.Files.readSymbolicLink(file));
                else if(attrs.isRegularFile()) {
                    File backing=PlaceholderFiles.resolve(file.toFile());
                    String unix=rel.toString().replace(File.separatorChar,'/');
                    if(unix.startsWith("objects/")||unix.startsWith("extra-data/"))java.nio.file.Files.createLink(target,backing.toPath());
                    else {
                        try(java.io.InputStream in=PlaceholderFiles.open(file.toFile())){java.nio.file.Files.copy(in,target);}
                        try {android.system.Os.chmod(target.toString(),0666);}catch(android.system.ErrnoException e){throw new IOException(e);}
                    }
                } else throw new IOException("Unsupported file in Flatpak repository: "+rel);
                return FileVisitResult.CONTINUE;
            }
        });
        try {android.system.Os.chmod(destination.toString(),0777);}catch(android.system.ErrnoException e){throw new IOException(e);}
    }
    private static void remove(File tree) {
        if(!java.nio.file.Files.isSymbolicLink(tree.toPath())&&tree.isDirectory()) {File[] children=tree.listFiles();if(children!=null)for(File child:children)remove(child);}
        //noinspection ResultOfMethodCallIgnored
        tree.delete();
    }
}
