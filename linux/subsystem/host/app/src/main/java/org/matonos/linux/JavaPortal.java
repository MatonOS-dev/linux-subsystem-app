package org.matonos.linux;

import android.app.UiModeManager;
import android.content.Context;
import android.content.Intent;
import android.content.ClipData;
import android.content.res.Configuration;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;
import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.function.Consumer;
import static org.matonos.linux.PortalWire.*;

/** One private backend channel per SessionBus, with no native Android logic. */
final class JavaPortal implements AutoCloseable,PortalBackend.Platform {
    interface Launcher {boolean launch(Intent intent) throws Exception;}
    private final Context context;
    private final Consumer<Boolean> changed;
    private final Launcher launcher;
    private final LocalSocket bound=new LocalSocket();
    private final LocalServerSocket server;
    private final File path;
    private final ShortSocketPath shortPath;
    private final HostCommandBackend commands;
    private final PortalBackend backend=new PortalBackend(this);
    private final byte[] secret=new byte[32];
    private final Set<Object> fileOwners=Collections.newSetFromMap(new IdentityHashMap<>());
    private volatile LocalSocket client;
    private volatile boolean stopped, held;
    private volatile PeerConnection connection;
    private volatile List<FileDescriptor> activeDescriptors=Collections.emptyList();
    private volatile File flatpakUserDir,flatpakSystemDir;
    JavaPortal(Context context,File directory,Consumer<Boolean> changed,Launcher launcher) throws Exception {
        this.context=context;this.changed=changed;this.launcher=launcher;
        android.content.pm.ApplicationInfo host=context.getPackageManager().getApplicationInfo("org.matonos.linux",0);
        File flatpakCommand=new File(host.nativeLibraryDir,"libmatonos-app-flatpak.so");
        String flatpakPath=flatpakCommand.isFile()&&flatpakCommand.canExecute()?flatpakCommand.getAbsolutePath():null;
        commands=new HostCommandBackend(new SandboxCommandSpawner(directory),message->{PeerConnection peer=connection;if(peer!=null)try{peer.send(message);}catch(IOException ignored){}},flatpakPath);
        new java.security.SecureRandom().nextBytes(secret);
        path=new File(directory,"portal-backend");if(path.exists()&&!path.delete())throw new IOException("Stale portal socket");
        shortPath=ShortSocketPath.open(directory);
        bound.bind(new LocalSocketAddress(shortPath.path("portal-backend"),LocalSocketAddress.Namespace.FILESYSTEM));Os.chmod(path.getAbsolutePath(),0600);
        server=new LocalServerSocket(bound.getFileDescriptor());new Thread(this::serve,"java-portals").start();
        new Thread(this::startFlatpakPortal,"flatpak-portal-start").start();
    }
    private void startFlatpakPortal() {
        File socket=new File(path.getParentFile(),"host-command");
        Log.i("MatonPortal","Waiting for compat-proxy command socket at "+socket);
        try {
            for(int attempt=0;attempt<300&&!stopped;attempt++) {
                if(socket.exists()) {
                    Log.i("MatonPortal","Compat-proxy command socket is ready");
                    // Dispatch the upstream portal applet directly. The
                    // ordinary app trampoline selects the CLI and interprets
                    // "portal" as a CLI command instead of an applet.
                    android.content.pm.ApplicationInfo host=context.getPackageManager()
                            .getApplicationInfo("org.matonos.linux",0);
                    File portal=new File(host.nativeLibraryDir,"libmatonos-flatpak.so");
                    if(!portal.isFile()||!portal.canExecute())
                        throw new IOException("Upstream Flatpak launcher is missing");
                    String portalPath=portal.getAbsolutePath();
                    List<byte[]> argv=Arrays.asList(nul(portalPath),nul("portal"),nul("--no-idle-exit"),nul("--verbose"));
                    Map<String,String> env=new LinkedHashMap<>();
                    env.put("PATH",portal.getParent()+":/system/bin:/system/xbin");
                    // The portal normally executes /usr/bin/flatpak for Spawn.
                    // Use the unprivileged trampoline so Spawn selects the CLI
                    // applet and preserves the matching nested bwrap executable.
                    env.put("FLATPAK",new File(portal.getParentFile(),
                            "libmatonos-app-flatpak.so").getAbsolutePath());
                    // The command runs in the captured Flatpak root, where
                    // PerAppRuntime publishes the stub runtime at /run/flatpak.
                    env.put("XDG_RUNTIME_DIR","/run/flatpak");
                    env.put("DBUS_SESSION_BUS_ADDRESS","unix:path=/run/flatpak/bus");
                    env.put("FLATPAK_BWRAP",new File(portal.getParentFile(),
                            "libmatonos-app-bwrap.so").getAbsolutePath());
                    if(new File(context.getCacheDir(),"flatpak-portal-debug").isFile())
                        env.put("G_MESSAGES_DEBUG","all");
                    File userDir=flatpakUserDir,systemDir=flatpakSystemDir;
                    if(userDir!=null)env.put("FLATPAK_USER_DIR",userDir.getAbsolutePath());
                    if(systemDir!=null)env.put("FLATPAK_SYSTEM_DIR",systemDir.getAbsolutePath());
                    try {commands.startFlatpakPortal(argv,env);Log.i("MatonPortal","Started upstream Flatpak portal in compat proxy");return;}
                    catch(Exception error) {Log.w("MatonPortal","Cannot start upstream Flatpak portal",error);}
                }
                try {Thread.sleep(100);}catch(InterruptedException error){Thread.currentThread().interrupt();return;}
            }
            if(!stopped)Log.w("MatonPortal","Compat-proxy command socket did not appear before timeout");
        } catch(Exception error) {
            if(!stopped)Log.w("MatonPortal","Upstream Flatpak portal unavailable",error);
        }
    }
    private static byte[] nul(String value) {
        byte[] text=value.getBytes(StandardCharsets.UTF_8),result=Arrays.copyOf(text,text.length+1);return result;
    }
    String secret() {StringBuilder s=new StringBuilder();for(byte b:secret)s.append(String.format(java.util.Locale.ROOT,"%02x",b&255));return s.toString();}
    boolean isHeld() {return held;}
    void setFlatpakDirectories(File userDir,File systemDir) {
        flatpakUserDir=userDir;flatpakSystemDir=systemDir;
        commands.setFlatpakDirectories(userDir==null?null:userDir.getAbsolutePath(),
                systemDir==null?null:systemDir.getAbsolutePath());
    }
    private static void readFully(InputStream in,byte[] data) throws IOException {
        int offset=0;while(offset<data.length){int n=in.read(data,offset,data.length-offset);if(n<=0)throw new EOFException();offset+=n;}
    }
    private void serve() {
        try {
            // The in-process broker connects exactly once at session start.
            // If that connection ever ends before this portal is closed (a
            // transient peer error, a reset broker channel), the backend and
            // the upstream flatpak-portal process must stay alive for the
            // stub's lifetime: a later payload launch still needs them. Log
            // loudly and accept a fresh authenticated broker instead of
            // tearing the whole portal down.
            while(!stopped) {
                LocalSocket socket=server.accept();client=socket;socket.setSoTimeout(3000);
                byte[] token=new byte[64];
                try {
                    if(socket.getPeerCredentials().getUid()!=android.os.Process.myUid())throw new IOException("Wrong backend UID");
                    byte[] magic=new byte[4];readFully(socket.getInputStream(),magic);
                    if(!Arrays.equals(magic,new byte[]{'M','B','P','1'}))throw new IOException("Unsupported portal protocol");
                    readFully(socket.getInputStream(),token);
                    if(!java.security.MessageDigest.isEqual(token,secret().getBytes(StandardCharsets.US_ASCII)))throw new IOException("Wrong backend capability");
                }catch(Exception e){socket.close();client=null;continue;}
                socket.getOutputStream().write(new byte[]{'O','K','A','Y'});
                socket.setSoTimeout(0);run(socket);
                if(!stopped)Log.w("MatonPortal","Portal broker connection ended; keeping the portal backend resident");
            }
        }catch(Exception e){if(!stopped)Log.w("MatonPortal","Backend disconnected",e);}
        finally {
            commands.close();
            try {backend.close();}catch(Exception e){Log.w("MatonPortal","Hold release failed",e);}
            synchronized(fileOwners){for(Object o:fileOwners)PortalFiles.release(context,o);fileOwners.clear();}
            try {if(client!=null)client.close();}catch(Exception ignored){}
            try {server.close();}catch(Exception ignored){}
            try {bound.close();}catch(Exception ignored){}
            try {shortPath.close();}catch(Exception ignored){}
        }
    }
    private void run(LocalSocket socket) throws Exception {
        PeerConnection peer=new PeerConnection(new LocalSocketChannel(socket),new Handler());
        connection=peer;
        try {peer.serve();} finally {connection=null;}
    }
    private final class Handler implements PeerConnection.Handler {
        @Override public List<Message> methodCall(Message call) {
            activeDescriptors=call.fds;
            try {return HostCommandBackend.PATH.equals(call.path)?commands.dispatch(call):backend.dispatch(call);}
            finally {activeDescriptors=Collections.emptyList();}
        }
        @Override public void replied(Message call){commands.replied(call);}
        @Override public void signal(Message message) {
            if("org.matonos.PortalBackend".equals(message.iface)&&"ClientClosed".equals(message.member))
                clientClosed(String.valueOf(message.body.get(0)));
        }
    }
    private void clientClosed(String closed){
        commands.disconnected(closed);
        try{backend.disconnected(closed);}catch(Exception e){Log.w("MatonPortal","Caller cleanup failed",e);}
        synchronized(fileOwners){Iterator<Object> it=fileOwners.iterator();while(it.hasNext()){Object o=it.next();if(o.toString().equals(closed)){PortalFiles.release(context,o);it.remove();}}}
    }
    @Override public Map<String,Map<String,Variant>> settings() {
        Map<String,Variant> appearance=new LinkedHashMap<>();
        int mode=context.getResources().getConfiguration().uiMode&Configuration.UI_MODE_NIGHT_MASK;
        UiModeManager ui=context.getSystemService(UiModeManager.class);
        if(ui!=null) {int night=ui.getNightMode();if(night==UiModeManager.MODE_NIGHT_YES)mode=Configuration.UI_MODE_NIGHT_YES;else if(night==UiModeManager.MODE_NIGHT_NO)mode=Configuration.UI_MODE_NIGHT_NO;}
        appearance.put("color-scheme",new Variant("u",mode==Configuration.UI_MODE_NIGHT_YES?1:mode==Configuration.UI_MODE_NIGHT_NO?2:0));
        appearance.put("contrast",new Variant("u",android.os.Build.VERSION.SDK_INT>=34&&ui!=null&&ui.getContrast()>0?1:0));
        try {
            if(android.os.Build.VERSION.SDK_INT<31)throw new android.content.res.Resources.NotFoundException();
            int color=context.getColor(android.R.color.system_accent1_500);
            appearance.put("accent-color",new Variant("(ddd)",Arrays.asList(((color>>16)&255)/255.0,((color>>8)&255)/255.0,(color&255)/255.0)));
        }catch(android.content.res.Resources.NotFoundException ignored){}
        Map<String,Map<String,Variant>> result=new LinkedHashMap<>();result.put("org.freedesktop.appearance",appearance);
        result.put("org.gnome.desktop.wm.preferences",Collections.singletonMap("button-layout",new Variant("s",":")));return result;
    }
    @Override public synchronized void hold(boolean active) {
        active=active&&!stopped;
        if(active==held)return;
        // The verified stub acquires/releases under its own UID via changed.
        held=active;
        try{changed.accept(active);}catch(RuntimeException e){Log.w("MatonPortal","Inhibit listener failed",e);}
    }
    @Override public boolean open(String caller,String method,Object target,Map<String,Variant> options) throws Exception {
        if(stopped)return false;
        Variant ask=options.get("ask");
        if(ask!=null && !"b".equals(ask.signature))return false;
        Uri uri;boolean writable=false;Object fileOwner=null;
        if("OpenURI".equals(method)) {
            uri=Uri.parse((String)target);String scheme=uri.getScheme();
            // Files must arrive by capability FD; never open host paths or
            // forward arbitrary Android provider URIs with host authority.
            if(scheme==null||scheme.equalsIgnoreCase("file")||scheme.equalsIgnoreCase("content")||scheme.equalsIgnoreCase("intent"))return false;
        }else {
            int index=(Integer)target;
            List<FileDescriptor> descriptors=activeDescriptors;
            FileDescriptor raw=index>=0&&index<descriptors.size()?descriptors.get(index):null;
            if(raw==null)return false;
            ParcelFileDescriptor source=ParcelFileDescriptor.dup(raw);
            try {
                android.system.StructStat stat=Os.fstat(source.getFileDescriptor());boolean directory="OpenDirectory".equals(method);
                if(directory?!OsConstants.S_ISDIR(stat.st_mode):!(OsConstants.S_ISREG(stat.st_mode)||OsConstants.S_ISDIR(stat.st_mode)))return false;
                directory=OsConstants.S_ISDIR(stat.st_mode);
                Variant write=options.get("writable");if(write!=null){if(!"b".equals(write.signature))return false;writable=(Boolean)write.value;}
                int flags=Os.fcntlInt(source.getFileDescriptor(),OsConstants.F_GETFL,0);
                if((flags&OsConstants.O_ACCMODE)==OsConstants.O_WRONLY || (writable&&(flags&OsConstants.O_ACCMODE)==OsConstants.O_RDONLY))return false;
                fileOwner=new String(caller);uri=PortalFiles.publish(context,source,directory,writable,fileOwner);
            } finally {source.close();}
            synchronized(fileOwners){if(stopped){PortalFiles.revoke(context,uri);return false;}fileOwners.add(fileOwner);}
        }
        Intent intent=new Intent(Intent.ACTION_VIEW).addCategory(Intent.CATEGORY_DEFAULT);
        if(fileOwner!=null) {
            intent.setDataAndType(uri,context.getContentResolver().getType(uri));
            intent.setClipData(ClipData.newRawUri("Linux document",uri));intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            if(writable)intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        }else intent.setData(uri);
        if(ask!=null && (Boolean)ask.value)intent=Intent.createChooser(intent,"Open with");
        try {
            boolean ok=launcher.launch(intent);
            if(!ok&&fileOwner!=null){PortalFiles.revoke(context,uri);synchronized(fileOwners){fileOwners.remove(fileOwner);}}
            return ok;
        }catch(Exception e){if(fileOwner!=null){PortalFiles.revoke(context,uri);synchronized(fileOwners){fileOwners.remove(fileOwner);}}return false;}
    }
    @Override public void close() {
        stopped=true;commands.close();
        PeerConnection peer=connection;if(peer!=null)peer.close();
        try{server.close();}catch(Exception ignored){}
        try{bound.close();}catch(Exception ignored){}
        try{if(client!=null)client.close();}catch(Exception ignored){}
        hold(false);synchronized(fileOwners){for(Object o:fileOwners)PortalFiles.release(context,o);fileOwners.clear();}path.delete();
    }
}
