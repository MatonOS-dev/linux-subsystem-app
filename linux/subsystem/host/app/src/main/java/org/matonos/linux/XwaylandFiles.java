package org.matonos.linux;

import android.content.Context;
import android.system.Os;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import org.json.JSONObject;

/** Data is private to the stub; executables always remain in the signed host APK's native directory. */
final class XwaylandFiles {
    static File prepare(Context app, File runtime) throws Exception {
        Context host=app.createPackageContext("org.matonos.linux",0);
        File nativeDir=new File(host.getApplicationInfo().nativeLibraryDir);
        if(!new File(nativeDir,"libmatonos-Xwayland.so").canExecute())
            throw new IOException("Host Xwayland executable is missing");
        long version=app.getPackageManager().getPackageInfo("org.matonos.linux",0).lastUpdateTime;
        File root=new File(runtime,"xwayland-"+version);
        File xkb=new File(root,"xkb");
        if(!new File(root,"ready").isFile()) {
            copy(host,"xwayland/xkb",xkb);
            new File(root,"ready").createNewFile();
        }
        File libraries=new File(root,"lib");
        if(!libraries.isDirectory()&&!libraries.mkdirs())throw new IOException("Cannot create Xwayland library directory");
        JSONObject links;
        try(java.io.InputStream in=host.getAssets().open("xwayland/libraries.json")) {
            links=new JSONObject(new String(in.readAllBytes(),java.nio.charset.StandardCharsets.UTF_8));
        }
        for(java.util.Iterator<String> it=links.keys();it.hasNext();) {
            String name=it.next();
            link(new File(nativeDir,links.getString(name)),new File(libraries,name));
        }
        File bin=new File(root,"bin");
        if(!bin.isDirectory()&&!bin.mkdirs())throw new IOException("Cannot create XKB compiler directory");
        link(new File(nativeDir,"libmatonos-xkbcomp.so"),new File(bin,"xkbcomp"));
        Os.setenv("MATON_XKB_BIN",bin.getAbsolutePath(),true);
        Os.setenv("PATH",bin.getAbsolutePath()+":/system/bin",true);
        Os.setenv("MATON_XKB_ROOT",xkb.getAbsolutePath(),true);
        Os.setenv("MATON_XWAYLAND_LIBDIR",libraries.getAbsolutePath()+":"+nativeDir.getAbsolutePath(),true);
        Os.setenv("XDG_RUNTIME_DIR",runtime.getAbsolutePath(),true);
        return nativeDir;
    }
    private static void link(File target,File link) throws Exception {
        try {Os.remove(link.getAbsolutePath());}
        catch(android.system.ErrnoException e){if(e.errno!=android.system.OsConstants.ENOENT)throw e;}
        Os.symlink(target.getAbsolutePath(),link.getAbsolutePath());
    }
    private static void copy(Context host,String asset,File output) throws IOException {
        String[] children=host.getAssets().list(asset);
        if(children!=null&&children.length>0) {
            if(!output.isDirectory()&&!output.mkdirs())throw new IOException("Cannot extract XKB data");
            for(String child:children)copy(host,asset+"/"+child,new File(output,child));
        } else {
            try(java.io.InputStream in=host.getAssets().open(asset)) {
                Files.copy(in,output.toPath(),java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            }
        }
    }
    private XwaylandFiles() { }
}
