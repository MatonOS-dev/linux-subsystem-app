package org.matonos.linux;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.FileNotFoundException;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/** Capability URIs for received FDs. Android URI grants authorize recipients;
 * the provider never resolves a Linux pathname using host privileges. */
public final class PortalFiles extends ContentProvider {
    private static String authority(android.content.Context context) { return context.getPackageName()+".portal.files"; }
    private static final android.os.HandlerThread ioThread=new android.os.HandlerThread("portal-file-io");
    static {ioThread.start();}
    private static final android.os.Handler io=new android.os.Handler(ioThread.getLooper());
    private static final Map<String,Entry> files=new ConcurrentHashMap<>();
    private static final class Entry {
        final ParcelFileDescriptor fd;
        final String mime, name;
        final boolean writable;
        final Object owner;
        Entry(ParcelFileDescriptor fd,String mime,boolean writable,Object owner) {this.fd=fd;this.mime=mime;this.name=name(fd);this.writable=writable;this.owner=owner;}
    }
    private static String name(ParcelFileDescriptor fd) {
        try {return new java.io.File(android.system.Os.readlink("/proc/self/fd/"+fd.getFd())).getName();}
        catch(Exception e){return "Linux document";}
    }
    static Uri publish(android.content.Context context,ParcelFileDescriptor fd,boolean directory,boolean writable,Object owner) throws Exception {
        String token=java.util.UUID.randomUUID().toString();
        if(files.size()>=256)throw new IllegalStateException("Too many portal files");
        String filename=name(fd);int dot=filename.lastIndexOf('.');
        String mime=dot<0?null:android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(filename.substring(dot+1).toLowerCase(java.util.Locale.ROOT));
        files.put(token,new Entry(ParcelFileDescriptor.dup(fd.getFileDescriptor()),directory?"vnd.android.document/directory":mime==null?"application/octet-stream":mime,writable,owner));
        return new Uri.Builder().scheme("content").authority(authority(context)).appendPath(token).build();
    }
    static void release(android.content.Context context,Object owner) {
        files.forEach((token,e)-> {if(e.owner==owner && files.remove(token,e)) {
            context.revokeUriPermission(Uri.parse("content://"+authority(context)+"/"+token),android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION|android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            synchronized(e){try {e.fd.close();}catch(Exception ignored){}}
        }});
    }
    static void revoke(android.content.Context context,Uri uri) {
        Entry e=files.remove(uri.getLastPathSegment());
        context.revokeUriPermission(uri,android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION|android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        if(e!=null)synchronized(e){try {e.fd.close();}catch(Exception ignored){}}
    }
    private Entry entry(Uri uri) throws FileNotFoundException {
        Entry e=files.get(uri.getLastPathSegment());if(e==null)throw new FileNotFoundException("Expired portal URI");return e;
    }
    @Override public boolean onCreate() {return true;}
    @Override public ParcelFileDescriptor openFile(Uri uri,String mode) throws FileNotFoundException {
        Entry e=entry(uri);
        if(!"r".equals(mode) && !(e.writable&&"rw".equals(mode)))throw new FileNotFoundException("Mode is not granted");
        // A proxy controls the recipient's access mode and independent offset
        // without reopening a cross-UID Linux pathname. dup(O_RDWR) alone
        // would let a recipient with a read URI grant write the source file.
        ParcelFileDescriptor source;
        try {synchronized(e){source=ParcelFileDescriptor.dup(e.fd.getFileDescriptor());}}
        catch(Exception ex){throw new FileNotFoundException("Descriptor unavailable");}
        try {
            return getContext().getSystemService(android.os.storage.StorageManager.class).openProxyFileDescriptor(
                "r".equals(mode)?ParcelFileDescriptor.MODE_READ_ONLY:ParcelFileDescriptor.MODE_READ_WRITE,
                new android.os.ProxyFileDescriptorCallback() {
                    @Override public long onGetSize() throws android.system.ErrnoException {
                        return android.system.Os.fstat(source.getFileDescriptor()).st_size;
                    }
                    @Override public int onRead(long offset,int size,byte[] data) throws android.system.ErrnoException {
                        try {return android.system.Os.pread(source.getFileDescriptor(),data,0,size,offset);}
                        catch(java.io.InterruptedIOException ex){throw new android.system.ErrnoException("pread",android.system.OsConstants.EINTR,ex);}
                    }
                    @Override public int onWrite(long offset,int size,byte[] data) throws android.system.ErrnoException {
                        if(!e.writable || !"rw".equals(mode))throw new android.system.ErrnoException("write",android.system.OsConstants.EACCES);
                        try {return android.system.Os.pwrite(source.getFileDescriptor(),data,0,size,offset);}
                        catch(java.io.InterruptedIOException ex){throw new android.system.ErrnoException("pwrite",android.system.OsConstants.EINTR,ex);}
                    }
                    @Override public void onFsync() throws android.system.ErrnoException {android.system.Os.fsync(source.getFileDescriptor());}
                    @Override public void onRelease() {try {source.close();}catch(Exception ignored){}}
                },io);
        }catch(Exception ex) {
            try {source.close();}catch(Exception ignored){}
            throw new FileNotFoundException("Cannot create portal file proxy");
        }
    }
    @Override public String getType(Uri uri) {try{return entry(uri).mime;}catch(Exception e){return null;}}
    @Override public Cursor query(Uri uri,String[] projection,String selection,String[] args,String order) {
        try {
            Entry e=entry(uri);String[] cols=projection==null?new String[]{OpenableColumns.DISPLAY_NAME,OpenableColumns.SIZE}:projection;
            MatrixCursor c=new MatrixCursor(cols);Object[] row=new Object[cols.length];
            for(int i=0;i<cols.length;i++) {
                if(OpenableColumns.DISPLAY_NAME.equals(cols[i]))row[i]=e.name;
                else if(OpenableColumns.SIZE.equals(cols[i]))row[i]=e.fd.getStatSize();
            }
            c.addRow(row);return c;
        }catch(Exception e){return null;}
    }
    @Override public Uri insert(Uri uri,ContentValues values) {throw new UnsupportedOperationException();}
    @Override public int update(Uri uri,ContentValues values,String selection,String[] args) {throw new UnsupportedOperationException();}
    @Override public int delete(Uri uri,String selection,String[] args) {throw new UnsupportedOperationException();}
}
