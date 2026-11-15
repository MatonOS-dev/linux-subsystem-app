package org.matonos.linux.store;

import android.content.*;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import java.io.*;
import java.util.*;

/** AppStream cached icons readable by the same permission-checked store client. */
public final class FlatpakIconProvider extends ContentProvider {
    @Override public boolean onCreate(){return true;}
    @Override public String getType(Uri uri){return "image/png";}
    @Override public ParcelFileDescriptor openFile(Uri uri,String mode) throws FileNotFoundException {
        try {
            if(!mode.equals("r"))throw new IOException("Read-only AppStream icon");
            List<String> parts=uri.getPathSegments();
            if(parts.size()!=2||!parts.get(0).matches("[A-Za-z0-9_][A-Za-z0-9_.-]*")||!parts.get(1).matches("[A-Za-z0-9_.+-]+\\.png"))throw new IOException("Invalid AppStream icon URI: "+uri);
            File active=new File(getContext().getNoBackupFilesDir(),"store-flatpak/installation/appstream/"+parts.get(0)+"/x86_64/active").getCanonicalFile();
            for(String size:Arrays.asList("128x128","64x64")) {
                File icon=new File(active,"icons/"+size+"/"+parts.get(1)).getCanonicalFile();
                if(icon.toPath().startsWith(active.toPath())&&icon.isFile())return ParcelFileDescriptor.open(icon,ParcelFileDescriptor.MODE_READ_ONLY);
            }
            throw new IOException("AppStream icon missing: "+uri);
        }catch(Exception error){Log.e("FlatpakSource","open icon uri="+uri+" failed",error);throw new FileNotFoundException(error.toString());}
    }
    @Override public Cursor query(Uri uri,String[] projection,String selection,String[] args,String sort){return null;}
    @Override public Uri insert(Uri uri,ContentValues values){throw new UnsupportedOperationException("Read-only AppStream icons");}
    @Override public int delete(Uri uri,String selection,String[] args){throw new UnsupportedOperationException("Read-only AppStream icons");}
    @Override public int update(Uri uri,ContentValues values,String selection,String[] args){throw new UnsupportedOperationException("Read-only AppStream icons");}
}
