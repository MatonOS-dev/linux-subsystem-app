package org.matonos.linux;

import android.app.Service;
import android.content.Intent;
import android.content.pm.*;
import android.os.*;
import android.util.Log;
import org.json.*;
import org.matonos.store.source.*;
import org.matonos.store.source.types.*;
import org.matonos.linux.store.*;
import org.matonos.linux.stub.StoreFlatpak;
import org.matonos.linuxhost.stubgen.*;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.*;

/** Software Centre source. The legacy ILinuxAppStore service remains independent. */
public final class FlatpakSourceService extends Service {
    private static final String TAG="FlatpakSource";
    private StoreFlatpak flatpak;
    private final SourceOperations operations=new SourceOperations();
    private List<Map<String,Object>> cachedCatalogue;
    private final Map<String,Boolean> brokenRemotes=new HashMap<>();
    private android.os.PowerManager.WakeLock installWakeLock;
    private synchronized void beginInstall() {
        android.app.NotificationManager manager=getSystemService(android.app.NotificationManager.class);
        manager.createNotificationChannel(new android.app.NotificationChannel("store-source-install","Software Centre downloads",android.app.NotificationManager.IMPORTANCE_LOW));
        startForeground(4,new android.app.Notification.Builder(this,"store-source-install").setContentTitle("Preparing Linux application")
                .setSmallIcon(android.R.drawable.stat_sys_download).setOngoing(true).build(),ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        if(installWakeLock==null)installWakeLock=getSystemService(android.os.PowerManager.class).newWakeLock(android.os.PowerManager.PARTIAL_WAKE_LOCK,"MatonOS:StoreSource");
        installWakeLock.acquire();
    }
    private synchronized void endInstall(String operation) {
        active.remove(operation);
        if(installWakeLock!=null&&installWakeLock.isHeld())installWakeLock.release();
        if(active.isEmpty())stopForeground(STOP_FOREGROUND_REMOVE);
    }
    private final Set<String> active=java.util.concurrent.ConcurrentHashMap.newKeySet();
    private interface Call<T> {T run() throws Exception;}
    private synchronized <T> T call(String operation,Call<T> body) {
        try {ensure();return body.run();}
        catch(IllegalArgumentException|IllegalStateException|SecurityException|UnsupportedOperationException error){Log.e(TAG,operation+" failed",error);throw error;}
        catch(Exception error){Log.e(TAG,operation+" failed",error);throw new IllegalStateException(operation+": "+error,error);}
    }
    private void ensure() throws Exception {
        if(flatpak!=null)return;
        StoreFlatpak candidate=new StoreFlatpak(this,new File(getNoBackupFilesDir(),"store-flatpak"));
        if(!new File(candidate.root,"configured").exists()){candidate.preset();Files.write(new File(candidate.root,"configured").toPath(),new byte[]{49});}
        flatpak=candidate;
    }
    private final ISoftwareSource.Stub binder=new ISoftwareSource.Stub() {
        @Override public SourceInfo info(){SourceInfo info=new SourceInfo();info.version=ISoftwareSource.VERSION;info.revision=ISoftwareSource.REVISION;info.name="flatpak";info.label="Flatpak";info.capabilities=CAP_REPOSITORIES|CAP_AGE_RATINGS|CAP_REVIEWS|CAP_PERMISSIONS;return info;}
        @Override public AppListing[] search(String term,int offset,int limit){return call("search term="+term,()->SourceTypes.listings(AppStreamCatalogue.search(catalogue(),term),offset,limit));}
        @Override public AppDetails details(String id,String repo){return call("details id="+id,()->{
            Map<String,Object> row=find(id,repo);String remote=(String)row.get("origin"),ref=(String)row.get("ref");
            String metadata=flatpak.cli("remote-info","--system","--show-metadata",remote,ref);
            List<String> permissions=new ArrayList<>();String section="";
            for(String line:metadata.split("\n")) {
                line=line.trim();if(line.startsWith("[")){section=line;continue;}
                if((section.equals("[Context]")||section.endsWith("Bus Policy]"))&&line.contains("="))permissions.add(section+" "+line);
            }
            row.put("permissions",permissions);
            Object rawRating=row.get("ageRating");
            if(rawRating instanceof Map) {
                Map<?,?> raw=(Map<?,?>)rawRating;Map<?,?> attributes=(Map<?,?>)raw.get("attributes");
                List<String> args=new ArrayList<>(Arrays.asList("--oars",(String)raw.get("type"),Locale.getDefault().toString()));
                JSONArray categories=new JSONArray();
                for(Map.Entry<?,?> attribute:attributes.entrySet()) {
                    args.add(attribute.getKey()+"="+attribute.getValue());
                    categories.put(new JSONObject().put("id",attribute.getKey()).put("title",attribute.getKey()).put("description","").put("level",attribute.getValue()));
                }
                String[] rating=flatpak.helper(args.toArray(new String[0])).split("\n",-1);
                if(rating.length<3)throw new IOException("Invalid AppStream age rating helper response");
                row.put("ageRating",new JSONObject().put("minimumAge",Integer.parseInt(rating[0])).put("system",rating[1]).put("label",rating[2]).put("categories",categories));
            }

            String[] sizes=flatpak.helper("--sizes",flatpak.installation.getAbsolutePath(),remote,ref).trim().split("\n");
            if(sizes.length!=2)throw new IOException("Invalid Flatpak size response");
            row.put("downloadSize",Long.parseLong(sizes[0]));row.put("installedSize",Long.parseLong(sizes[1]));
            return SourceTypes.details(new JSONObject(row));
        });}
        @Override public Review[] reviews(String id,int offset,int limit){return call("reviews id="+id,()->SourceTypes.reviews(new JSONArray(odrs(id)),offset,limit));}
        @Override public AppListing[] installed(int offset,int limit){return call("installed",()->SourceTypes.listings(installedListings(false),offset,limit));}
        @Override public AppListing[] updates(int offset,int limit){return call("updates",()->SourceTypes.listings(installedListings(true),offset,limit));}
        @Override public Repo[] repos(int offset,int limit){return call("repos",()->SourceTypes.repos(repositories(),offset,limit));}
        @Override public void addRepo(String url){call("addRepo url="+url,()->{
            URI uri=URI.create(url);if(!"https".equals(uri.getScheme()))throw new IOException("Repository descriptor requires HTTPS");
            String name="store-"+java.util.HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(url.getBytes(StandardCharsets.UTF_8)));
            flatpak.cli("remote-add","--system","--if-not-exists",name,url);cachedCatalogue=null;return "";
        });}
        @Override public void delRepo(String id){call("delRepo id="+id,()->{validateRemote(id);flatpak.cli("remote-delete","--system",id);cachedCatalogue=null;return "";});}
        @Override public void cancel(String operation){operations.cancel(operation);}
        @Override public boolean running(String operation){return operations.running(operation);}
        @Override public void install(String id,String repo,String operation,IInstallCallback callback){
            if(callback==null)throw new IllegalArgumentException("Callback required");
            Map<String,Object> candidate=SourceCandidates.install(id,repo);
            SourceOperations.Operation op=operations.start(operation);
            IInstallCallback terminal=new IInstallCallback.Stub(){
                @Override public void progress(String key,String phase,int percent,long done,long total)throws RemoteException{if(!op.cancelled())callback.progress(key,phase,percent,done,total);}
                @Override public void commit(String key,String pkg,String[] names,ParcelFileDescriptor[] files,String[] remove,ICommitResult result)throws RemoteException{op.check();callback.commit(key,pkg,names,files,remove,result);}
                @Override public void done(String key)throws RemoteException{if(op.claimTerminal())try{if(op.cancelled())callback.failed(key,"cancelled");else callback.done(key);}finally{op.finish();}}
                @Override public void failed(String key,String error)throws RemoteException{if(op.claimTerminal())try{callback.failed(key,op.cancelled()?"cancelled":error);}finally{op.finish();}}
            };
            active.add(operation);
            Thread worker=new Thread(()->{
                boolean began=false;
                try {
                    beginInstall();began=true;op.check();
                    synchronized(FlatpakSourceService.this){ensure();}
                    op.check();StorePackages.install(FlatpakSourceService.this,flatpak,candidate,operation,terminal,op);
                }catch(Exception error){Log.e(TAG,"install id="+id+" repo="+repo+" op="+operation+" failed",error);try{terminal.failed(operation,error.toString());}catch(RemoteException delivery){Log.e(TAG,"failed callback op="+operation,delivery);}}
                finally{op.finish();if(began)endInstall(operation);else active.remove(operation);}
            },"flatpak-source-install");
            op.worker=worker;worker.start();
        }

    };
    @Override public IBinder onBind(Intent intent){return binder;}
    private JSONArray repositories() throws Exception {
        JSONArray result=new JSONArray();
        for(String line:flatpak.cli("remotes","--system","--columns=name,url,title").split("\n")) {
            if(line.isBlank())continue;String[] fields=line.split("\t",-1);if(fields.length!=3)throw new IOException("Invalid Flatpak remote row: "+line);
            result.put(new JSONObject().put("id",fields[0]).put("url",fields[1]).put("label",fields[2]).put("official",fields[0].equals("flathub")&&fields[1].equals("https://dl.flathub.org/repo/")).put("broken",brokenRemotes.getOrDefault(fields[0],false)));
        }
        return result;
    }
    private List<Map<String,Object>> catalogue() throws Exception {
        if(cachedCatalogue!=null)return cachedCatalogue;
        List<Map<String,Object>> result=new ArrayList<>();JSONArray remotes=repositories();Exception last=null;int readable=0;
        for(int i=0;i<remotes.length();i++) {
            String remote=remotes.getJSONObject(i).getString("id");validateRemote(remote);
            try {flatpak.cli("update","--system","--appstream","--noninteractive",remote);brokenRemotes.put(remote,false);}
            catch(Exception error){Log.e(TAG,"AppStream refresh remote="+remote+" failed; checking cached data",error);brokenRemotes.put(remote,true);last=error;}
            File active=new File(flatpak.installation,"appstream/"+remote+"/x86_64/active");File xml=new File(active,"appstream.xml.gz");
            boolean gzip=xml.isFile();if(!gzip)xml=new File(active,"appstream.xml");
            if(!xml.isFile()) {
                IOException missing=new IOException("AppStream XML missing remote="+remote+" active="+active);
                Log.e(TAG,missing.getMessage());brokenRemotes.put(remote,true);last=missing;continue;
            }
            readable++;
            try(InputStream input=new FileInputStream(xml)) {for(Map<String,Object> row:AppStreamCatalogue.read(input,gzip,remote)) {
                String icon=(String)row.get("icon");
                if(icon.startsWith("cached:"))row.put("icon",new android.net.Uri.Builder().scheme("content").authority("org.matonos.linux.store.icons")
                        .appendPath(remote).appendPath(icon.substring(7)).build().toString());
                row.put("id",row.get("ref"));
                row.put("packageName",org.matonos.linux.stub.DeployManifest.packageForPublic((String)row.get("ref")));result.add(row);
            }}
        }
        if(readable==0&&last!=null)throw last;
        cachedCatalogue=result;return result;
    }
    private Map<String,Object> find(String id) throws Exception {return find(id,null);}
    private Map<String,Object> find(String id,String repo) throws Exception {
        return SourceCandidates.find(catalogue(),id,repo);
    }

    private JSONArray installedListings(boolean updates) throws Exception {
        if(updates)cachedCatalogue=null;
        Set<String> selected=flatpak.installedRefs(updates);
        List<Map<String,Object>> catalogue;
        try{catalogue=catalogue();}catch(Exception error){
            Log.e(TAG,"Cannot decorate installed refs with AppStream; retaining signed installed listings",error);catalogue=Collections.emptyList();
        }
        JSONArray result=new JSONArray();
        for(PackageInfo pkg:getPackageManager().getInstalledPackages(PackageManager.GET_SIGNING_CERTIFICATES)) {
            if(!trusted(pkg))continue;
            DeploymentRecord record;
            try{record=DeploymentRecord.installed(pkg.splitNames,pkg.applicationInfo.splitSourceDirs);}
            catch(IOException incomplete){Log.w(TAG,"Incomplete installed carrier package="+pkg.packageName,incomplete);continue;}
            String[] parts=record.ref.split("/");if(!parts[0].equals("app"))continue;
            RemoteOrigin origin=RemoteOrigin.parse(record.remote);
            if(!selected.contains(record.ref))continue;
            Map<String,Object> listing=new LinkedHashMap<>();for(Map<String,Object> row:catalogue)if(record.ref.equals(row.get("ref"))&&origin.name.equals(row.get("origin"))){listing.putAll(row);break;}
            listing.put("id",record.ref);listing.put("name",listing.getOrDefault("name",parts[1]));listing.put("origin",origin.name);listing.put("packageName",pkg.packageName);listing.put("installed",true);
            result.put(new JSONObject(AppStreamCatalogue.search(Collections.singletonList(listing),"").get(0)));
        }
        return result;
    }
    public static boolean trusted(PackageInfo pkg) throws Exception {
        if(pkg.packageName==null||!pkg.packageName.startsWith("flatpak.")||pkg.signingInfo==null||pkg.signingInfo.hasMultipleSigners())return false;
        Signature[] signatures=pkg.signingInfo.getApkContentsSigners();byte[] expected=StubGenerator.getExistingSigningCertificate();
        return expected!=null&&signatures.length==1&&MessageDigest.isEqual(expected,signatures[0].toByteArray());
    }
    private String odrs(String id) throws Exception {
        Map<String,Object> app=find(id);String appId=((String)app.get("ref")).split("/")[1];
        JSONObject request=new JSONObject().put("app_id",appId).put("locale",Locale.getDefault().toString()).put("version",String.valueOf(app.getOrDefault("versionName","unknown")).isEmpty()?"unknown":app.getOrDefault("versionName","unknown"))
                .put("user_hash",java.util.HexFormat.of().formatHex(MessageDigest.getInstance("SHA-1").digest(getPackageName().getBytes(StandardCharsets.UTF_8))))
                .put("distro","MatonOS").put("limit",-1);
        HttpURLConnection connection=(HttpURLConnection)new URL("https://odrs.gnome.org/1.0/reviews/api/fetch").openConnection();
        try {
            connection.setRequestMethod("POST");connection.setDoOutput(true);connection.setRequestProperty("Content-Type","application/json");
            try(OutputStream out=connection.getOutputStream()){out.write(request.toString().getBytes(StandardCharsets.UTF_8));}
            if(connection.getResponseCode()!=200)throw new IOException("ODRS HTTP "+connection.getResponseCode()+" app="+id);
            JSONArray raw;try(InputStream in=connection.getInputStream()){raw=new JSONArray(new String(in.readAllBytes(),StandardCharsets.UTF_8));}
            JSONArray result=new JSONArray();for(int i=0;i<raw.length();i++){JSONObject review=raw.getJSONObject(i);if(!review.has("review_id"))continue;result.put(new JSONObject()
                    .put("id",String.valueOf(review.get("review_id"))).put("author",review.optString("user_display"))
                    .put("rating",review.getDouble("rating")/20.0).put("summary",review.optString("summary"))
                    .put("description",review.optString("description")).put("version",review.optString("version"))
                    .put("date",java.time.Instant.ofEpochSecond(review.getLong("date_created")).toString()));}return result.toString();
        }finally{connection.disconnect();}
    }
    private static void validateRemote(String remote) throws IOException {if(remote==null||!remote.matches("[A-Za-z0-9_.-]+")||remote.startsWith("-"))throw new IOException("Invalid remote name: "+remote);}
}
