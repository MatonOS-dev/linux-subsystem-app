package org.matonos.linux.stub;

import android.os.Binder;
import android.os.IBinder;
import android.util.Log;
import org.matonos.erofs.IErofsMounter;
import java.io.File;
import java.io.IOException;
import java.util.Arrays;
import java.util.List;
import java.util.regex.Pattern;

/** Mounts the one signed deployment image for a package before proxy start. */
final class ErofsLaunch {
    private static final String TAG="MatonErofsLaunch";
    private static final IBinder TOKEN=new Binder();
    private static final Pattern IMAGE_SPLIT=Pattern.compile("erofs_[0-9a-f]{64}");
    private ErofsLaunch(){}

    static List<String> arguments(File base,String[] names,String[] paths,File cache,String target,String subdirectory) throws Exception {
        if(target==null||!target.matches("/(?:[A-Za-z0-9_.+-]+/?)*"))throw new IOException("Invalid EROFS deployment target");
        if(names==null||paths==null||names.length!=paths.length)throw new IOException("Missing package split paths for EROFS image");
        String path=mountInstalledImage(names,paths,TOKEN);
        if(subdirectory!=null&&!subdirectory.isEmpty()) {
            if(!subdirectory.matches("[A-Za-z0-9_.+-]+"))throw new IOException("Invalid EROFS branch directory");
            path=path+"/"+subdirectory;
        }
        Log.i(TAG,"Mounted one EROFS image at "+target+" from "+path);
        return Arrays.asList("--erofs-mount",target,path);
    }

    static String mountInstalledImage(String[] names,String[] paths,IBinder token) throws Exception {
        if(names==null||paths==null||names.length!=paths.length)throw new IOException("Missing package split paths for EROFS image");
        String split=null,apk=null;
        for(int i=0;i<names.length;i++)if(IMAGE_SPLIT.matcher(names[i]).matches()) {
            if(split!=null)throw new IOException("Package contains more than one EROFS image split");
            split=names[i];apk=paths[i];
        }
        if(split==null||apk==null)throw new IOException("Package lacks its EROFS image split");
        String path=mounter().mountSplit(apk,"assets/matonos/deployment.erofs",token);
        if(path==null||!path.startsWith("/mnt/matonos/"))throw new IOException("EROFS daemon returned an invalid mount path for "+split);
        return path;
    }

    private static IErofsMounter mounter() throws Exception {
        IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"matonos.erofs");
        IErofsMounter result=IErofsMounter.Stub.asInterface(binder);
        if(result==null)throw new IOException("matonos-erofsd is unavailable");return result;
    }
}
