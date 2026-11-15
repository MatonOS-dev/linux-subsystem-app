package org.matonos.linux.stub;

import android.content.*;
import android.os.*;
import android.util.Log;
import java.io.*;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.CompletableFuture;
import java.util.zip.ZipFile;
import org.json.JSONObject;
import org.matonos.linux.store.CommitChain;
import org.matonos.linuxhost.stubgen.*;

/** The store has installed the real stub before any vendor apply_extra runs. */
public final class StoreExtraData {
    private static final String TAG="FlatpakSource";
    public static boolean required(String metadata) {
        return java.util.regex.Pattern.compile("(?m)^\\s*\\[Extra Data\\]\\s*$").matcher(metadata).find();
    }
    public static CommitChain.Package prepare(Context context,StoreFlatpak staging,String ref,String remote,
            JSONObject identity,String metadata,File artifacts,String previousImage,int revision) throws Exception {
        String pkg=DeployManifest.packageForPublic(ref);
        Log.i(TAG,"extra-data begin package="+pkg+" ref="+ref);
        File returned=new File(artifacts,"stub-extra.apk"),image=new File(artifacts,"extra.erofs");
        CompletableFuture<IBinder> bound=new CompletableFuture<>();
        ServiceConnection connection=new ServiceConnection(){
            @Override public void onServiceConnected(ComponentName name,IBinder binder){bound.complete(binder);}
            @Override public void onServiceDisconnected(ComponentName name){
                Log.e(TAG,"extra-data service disconnected package="+pkg);
                bound.completeExceptionally(new IOException("Extra-data service disconnected package="+pkg));
            }
            @Override public void onNullBinding(ComponentName name){bound.completeExceptionally(new IOException("Null extra-data binding package="+pkg));}
            @Override public void onBindingDied(ComponentName name){onServiceDisconnected(name);}
        };
        Intent intent=new Intent(ExtraDataInstallService.ACTION).setComponent(new ComponentName(pkg,ExtraDataInstallService.class.getName()));
        if(!context.bindService(intent,connection,Context.BIND_AUTO_CREATE|Context.BIND_IMPORTANT))
            throw new IOException("Cannot bind installed extra-data stub package="+pkg);
        try {
            IBinder binder=bound.get();
            ParcelFileDescriptor[] pipe=ParcelFileDescriptor.createPipe();
            IBinder.DeathRecipient death=()->{
                Log.e(TAG,"extra-data stub died package="+pkg);
                close(pipe[0],pkg);close(pipe[1],pkg);
            };
            boolean linked=false;
            try {
                binder.linkToDeath(death,0);linked=true;
                Parcel request=Parcel.obtain();
                try {
                    request.writeFileDescriptor(pipe[1].getFileDescriptor());request.writeString(ref);request.writeString(remote);
                    RemoteOrigin origin=RemoteOrigin.parse(staging.origin(remote));
                    request.writeString(origin.url.isEmpty()?"https://dl.flathub.org/repo/":origin.url);
                    List<String> paths=new ArrayList<>();org.json.JSONArray subpaths=identity.getJSONArray("subpaths");
                    for(int i=0;i<subpaths.length();i++)paths.add(subpaths.getString(i));
                    request.writeString(String.join("\n",paths));
                    String runtime=runtime(metadata);request.writeString(runtime);
                    String descriptor=origin.flatpakrepo();
                    if(origin.url.isEmpty())try(InputStream in=context.getAssets().open("flatpak/remotes.d/flathub.flatpakrepo")) {
                        descriptor=new String(in.readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
                    }
                    request.writeString(descriptor);
                    if(!binder.transact(3,request,null,0))throw new IOException("Installed stub rejected extra-data request package="+pkg);
                } finally {request.recycle();}
                close(pipe[1],pkg);
                int readNumber=pipe[0].getFd();
                try(DataInputStream in=new DataInputStream(new ParcelFileDescriptor.AutoCloseInputStream(pipe[0]));
                        OutputStream out=Files.newOutputStream(returned.toPath())) {
                    try {
                        if(in.readInt()!=0)throw new IOException("Stub apply_extra failed package="+pkg+": "+in.readUTF());
                        long left=in.readLong();if(left<=0)throw new IOException("Invalid returned split size package="+pkg);
                        byte[] buffer=new byte[65536];
                        while(left>0){int count=in.read(buffer,0,(int)Math.min(left,buffer.length));if(count<0)throw new EOFException("Truncated extra-data split package="+pkg);out.write(buffer,0,count);left-=count;}
                    } finally {
                        Log.i(TAG,"close fd="+readNumber+" at StoreExtraData.prepare package="+pkg);
                    }
                }
                try(ZipFile zip=new ZipFile(PlaceholderFiles.resolve(returned))) {
                    java.util.zip.ZipEntry entry=zip.getEntry("assets/matonos/deployment.erofs");
                    if(entry==null)throw new IOException("Returned split lacks deployment image package="+pkg);
                    try(InputStream in=zip.getInputStream(entry);OutputStream out=Files.newOutputStream(image.toPath())){in.transferTo(out);}
                }
                File repo=new File(staging.installation,"repo");
                OstreePacks original=OstreePacks.read(repo,identity.getString("appCommit"),
                        StagedPacks.command(context,staging.root,repo,()->false),(file,name)->{
                            try{return android.system.Os.getxattr(file.getAbsolutePath(),name);}
                            catch(android.system.ErrnoException e){
                                if(e.errno==android.system.OsConstants.ENODATA||e.errno==android.system.OsConstants.ENOENT)return null;
                                throw new IOException("Cannot read OSTree xattr package="+pkg+" path="+file+" name="+name,e);
                            }
                        },paths(identity));
                List<ExtraImageValidator.Entry> trusted=new ArrayList<>();
                for(ObjectPacks.Entry entry:original.entries)trusted.add(new ExtraImageValidator.Entry(entry.path,entry.kind,entry.mode,entry.target,entry.checksum,entry.source));
                String prefix=ref.startsWith("app/")?"/app":"/"+ref.substring(ref.lastIndexOf('/')+1);
                File extracted=new File(artifacts,"extra-validated");
                try {
                    ExtraImageValidator.checkImage(LoaderCommand.tool(context,"libmatonos-fsck-erofs.so"),image,extracted,
                            ExtraImageValidator.flatpakFiles(trusted,prefix),prefix+"/extra");
                } finally {if(extracted.exists())org.matonos.linux.WorkDirs.remove(extracted);}
                // Rebuild the APK envelope from validated bytes; never trust a stub-supplied manifest.
                String split=StubGenerator.erofsSplitName(image);
                StubGenerator.generateErofsPack(new File(artifacts,"signing"),pkg,image,new File(artifacts,split+".apk"),revision);
                Parcel cleanup=Parcel.obtain();
                try {if(!binder.transact(2,cleanup,null,0))throw new IOException("Stub refused validated handback cleanup package="+pkg);}
                finally {cleanup.recycle();}
                Log.i(TAG,"extra-data validated package="+pkg+" ref="+ref+" split="+split);
                return new CommitChain.Package(pkg,Collections.emptyList(),new String[]{split+".apk"},
                        split.equals(previousImage)?new String[0]:new String[]{previousImage});
            } finally {if(linked)binder.unlinkToDeath(death,0);close(pipe[0],pkg);close(pipe[1],pkg);}
        } catch(Exception error){Log.e(TAG,"extra-data failed package="+pkg+" ref="+ref,error);throw error;}
        finally {context.unbindService(connection);}
    }
    private static List<String> paths(JSONObject identity) throws Exception {
        List<String> result=new ArrayList<>();org.json.JSONArray paths=identity.getJSONArray("subpaths");
        for(int i=0;i<paths.length();i++)result.add(paths.getString(i));return result;
    }
    private static String runtime(String metadata) {
        String section="";
        for(String line:metadata.split("\n")) {
            line=line.trim();if(line.startsWith("["))section=line;
            if((section.equals("[Application]")||section.equals("[ExtensionOf]"))&&line.startsWith("runtime=")) {
                String value=line.substring(8);return value.startsWith("runtime/")?value:"runtime/"+value;
            }
        }
        return "";
    }
    private static void close(ParcelFileDescriptor fd,String pkg) {
        int number;try{number=fd.getFd();}catch(IllegalStateException alreadyClosed){return;}
        Log.i(TAG,"close fd="+number+" at StoreExtraData.close package="+pkg);
        try{fd.close();}catch(IOException error){Log.e(TAG,"extra-data close failed package="+pkg,error);}
    }
    private StoreExtraData() { }
}
