package org.matonos.linux;

import android.app.Service;
import android.content.BroadcastReceiver;
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
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;

/** Software Centre source; installation is committed by the bound store. */
public final class FlatpakSourceService extends Service {
    private static final String TAG="FlatpakSource";
    private StoreFlatpak flatpak;
    private final SourceOperations operations=new SourceOperations();
    private volatile List<Map<String,Object>> cachedCatalogue;
    private volatile File catalogueFile;
    private volatile long catalogueFileVersion=-1;
    private volatile long catalogueFileCheckedAt;
    // Guarded by binder.catalogue()'s lock; network-derived data refreshed on the catalogue interval.
    private boolean appStreamFetchDue=true;private Map<String,Long> addedCache;private Map<String,Double> popularityCache;
    private final java.util.concurrent.atomic.AtomicLong catalogueGeneration=new java.util.concurrent.atomic.AtomicLong(1);
    private final Object catalogueLock=new Object();
    private CompletableFuture<List<Map<String,Object>>> catalogueLoad;
    private final Object repositoryMetadataLock=new Object();
    private volatile RepositoryMetadata cachedRepositoryMetadata;
    private final Object mutationLock=new Object();
    private final Object updateLock=new Object();
    private final Map<String,Boolean> verifiedApps=new java.util.concurrent.ConcurrentHashMap<>();
    private final Map<String,Boolean> brokenRemotes=new java.util.concurrent.ConcurrentHashMap<>();
    private android.os.PowerManager.WakeLock installWakeLock;
    private BroadcastReceiver packageReceiver;
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
    @Override public void onCreate() {
        super.onCreate();
        catalogueGeneration.set(getSharedPreferences("store-catalogue",MODE_PRIVATE).getLong("version",1));
        packageReceiver=new BroadcastReceiver() {
            @Override public void onReceive(android.content.Context context,Intent intent) { invalidateUpdates();invalidateRepositoryMetadata(); }
        };
        android.content.IntentFilter filter=new android.content.IntentFilter();
        filter.addAction(Intent.ACTION_PACKAGE_ADDED);filter.addAction(Intent.ACTION_PACKAGE_REMOVED);filter.addAction(Intent.ACTION_PACKAGE_REPLACED);
        filter.addDataScheme("package");
        registerReceiver(packageReceiver,filter,android.content.Context.RECEIVER_NOT_EXPORTED);
    }
    @Override public void onDestroy() {
        if(packageReceiver!=null)unregisterReceiver(packageReceiver);
        super.onDestroy();
    }
    private interface Call<T> {T run() throws Exception;}
    private <T> T call(String operation,Call<T> body) {
        try {ensure();return body.run();}
        catch(IllegalArgumentException|IllegalStateException|SecurityException|UnsupportedOperationException error){Log.e(TAG,operation+" failed",error);throw error;}
        catch(Exception error){Log.e(TAG,operation+" failed",error);throw new IllegalStateException(operation+": "+error,error);}
    }
    private synchronized void ensure() throws Exception {
        if(flatpak!=null)return;
        // A killed process leaves its operation trees behind; this fresh process holds no leases yet.
        for(String name:new String[]{"store-installs","store-queries"})WorkDirs.sweep(WorkDirs.directory(this,name));
        StoreFlatpak candidate=new StoreFlatpak(this,new File(getNoBackupFilesDir(),"store-flatpak"));
        if(!new File(candidate.root,"configured").exists()){candidate.preset();Files.write(new File(candidate.root,"configured").toPath(),new byte[]{49});}
        seedDefaultRemotes(candidate);
        flatpak=candidate;
    }
    private void seedDefaultRemotes(StoreFlatpak candidate) {
        String[][] defaults={{"flathub","https://dl.flathub.org/repo/flathub.flatpakrepo"},{"flatpark","https://dl.flatpark.org/flatpark.flatpakrepo"}};
        for(String[] remote:defaults) {
            String name=remote[0];File descriptor=new File(candidate.root,name+".flatpakrepo");
            try {
                byte[] contents=readRemoteDescriptor(name,remote[1]);Files.write(descriptor.toPath(),contents);
                candidate.cli("remote-add","--system","--if-not-exists","--from",name,descriptor.getAbsolutePath());
                String key=flatpakRepoField(new String(contents,StandardCharsets.UTF_8),"GPGKey");
                if(key==null||key.isBlank())throw new IOException("Remote descriptor has no GPGKey remote="+name+" url="+remote[1]+" received="+descriptorSummary(contents));
                File keyFile=new File(candidate.root,name+".gpg");Files.write(keyFile.toPath(),Base64.getMimeDecoder().decode(key));
                try {candidate.cli("remote-modify","--system","--gpg-import="+keyFile.getAbsolutePath(),name);}
                finally {if(keyFile.exists()&&!keyFile.delete())Log.e(TAG,"Cannot delete refreshed remote key file remote="+name+" path="+keyFile);}
                candidate.cli("remote-modify","--system","--prio="+(name.equals("flathub")?"1":"0"),name);
            } catch(Exception error) { Log.e(TAG,"Refreshing default remote descriptor/trust remote="+name+" url="+remote[1]+" failed",error); }
        }
    }
    private byte[] readRemoteDescriptor(String name,String url) throws Exception {
        URLConnection connection=new URL(url).openConnection();connection.setConnectTimeout(5000);connection.setReadTimeout(10000);
        try(InputStream input=connection.getInputStream()) {return input.readAllBytes();}
    }
    private static String descriptorSummary(byte[] contents) {
        String received=new String(contents,StandardCharsets.UTF_8).replaceAll("GPGKey=[^\\r\\n]+","GPGKey=<redacted>");
        if(received.length()>512)received=received.substring(0,512)+"…";
        return "bytes="+contents.length+" text="+JSONObject.quote(received);
    }
    private static String flatpakRepoField(String descriptor,String field) throws IOException {
        boolean section=false;String prefix=field+"=";
        for(String line:descriptor.split("\\R")) {
            String value=line.trim();if(value.startsWith("[")&&value.endsWith("]")){section=value.equals("[Flatpak Repo]");continue;}
            if(section&&value.startsWith(prefix))return value.substring(prefix.length()).trim();
        }
        return null;
    }
    private final ISoftwareSource.Stub binder=new ISoftwareSource.Stub() {
        @Override public SourceInfo info(){SourceInfo info=new SourceInfo();info.version=ISoftwareSource.VERSION;info.revision=ISoftwareSource.REVISION;info.name="flatpak";info.label="Flatpak";info.description="Desktop applications from configured software repositories.";info.capabilities=CAP_AGE_RATINGS|CAP_PERMISSIONS|CAP_LOCAL_FILES;return info;}
        @Override public synchronized ParcelFileDescriptor catalogue(){return call("catalogue",()->{
            File file=new File(getFilesDir(),"store-catalogue.json");long generation=catalogueGeneration.get(),now=System.currentTimeMillis();boolean refreshExpired=file.isFile()&&now-catalogueFileCheckedAt>=6L*60*60*1000;if(file.isFile()&&catalogueFileVersion==generation&&!refreshExpired)return ParcelFileDescriptor.open(file,ParcelFileDescriptor.MODE_READ_ONLY);
            // Package changes only change installed/update state; AppStream is fetched on the refresh interval.
            appStreamFetchDue=!file.isFile()||refreshExpired;
            File temp=new File(getFilesDir(),"store-catalogue.json.tmp");
            try(FileOutputStream output=new FileOutputStream(temp,false);OutputStreamWriter writer=new OutputStreamWriter(output,StandardCharsets.UTF_8)){
                StoreCatalogueWriter.write(writer,getPackageName(),System.currentTimeMillis()/1000L,FlatpakSourceService.this::writeCatalogueRows);
            }
            if(!temp.renameTo(file)){Log.e(TAG,"Cannot publish catalogue file source="+getPackageName()+" temp="+temp+" target="+file);throw new IOException("Cannot publish Flatpak catalogue");}
            if(refreshExpired)invalidateCatalogue();catalogueFile=file;catalogueFileVersion=catalogueGeneration.get();catalogueFileCheckedAt=now;return ParcelFileDescriptor.open(file,ParcelFileDescriptor.MODE_READ_ONLY);
        });}
        @Override public String[] unusedPackages(){return call("unusedPackages",()->{ensure();synchronized(mutationLock){return flatpak.unusedPackages().toArray(new String[0]);}});}
        @Override public long catalogueVersion(){return catalogueGeneration.get();}
        @Override public AppDetails details(String id,String repo){return call("details id="+id,()->{
            Map<String,Object> row=find(id,repo);row=withRepositoryOptions(Collections.singletonList(row)).get(0);String remote=(String)row.get("origin"),ref=(String)row.get("ref");
            String metadata=flatpak.cli("remote-info","--system","--show-metadata",remote,ref);
            row.put("permissions",StubGenerator.permissionsForMetadata(metadata));
            row.put("descriptionMarkdown",row.getOrDefault("descriptionMarkdown",row.get("description")));
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
            addVerifiedBadge(row);return SourceTypes.details(new JSONObject(row));
        });}
        @Override public Review[] reviews(String id,int offset,int limit){return new Review[0];}
        @Override public SourceSetting[] settings(){return call("settings",()->{
            List<SourceSetting> result=new ArrayList<>();
            SourceSetting beta=new SourceSetting();beta.id="flathub_beta";beta.label="Enable Flathub Beta (testing versions)";beta.type="boolean";
            JSONArray remotes=repositories();boolean betaEnabled=false;for(int i=0;i<remotes.length();i++){JSONObject remote=remotes.getJSONObject(i);if(remote.optString("id").equals("flathub-beta"))betaEnabled=remote.optBoolean("enabled",true);}
            beta.value=Boolean.toString(betaEnabled);result.add(beta);
            SourceSetting add=setting("add_repository","Add repository","action",null);SourceSetting url=setting("url","Repository URL","text",null);SourceSetting name=setting("name","Name","text",null);add.fields=new SourceSetting[]{url,name};result.add(add);
            for(int i=0;i<remotes.length();i++){JSONObject remote=remotes.getJSONObject(i);String id=remote.optString("id");SourceSetting group=setting("repository:"+id,remote.optString("label",id),"group",null);group.removable=!id.equals("flathub")&&!id.equals("flatpark")&&!id.equals("flathub-beta");
                SourceSetting enabled=setting("remote-enabled:"+id,"Enabled","boolean",Boolean.toString(remote.optBoolean("enabled",true)));
                String base="/var/lib/flatpak/repo/refs/remotes/"+id;File appstream=new File(base,"appstream.xml");int count=-1;
                if(cachedCatalogue!=null){count=0;for(Map<String,Object> app:cachedCatalogue)if(id.equals(app.get("origin")))count++;}
                File index=new File(base,"appstream.xml");long refreshed=index.isFile()?index.lastModified()/1000L:0;
                SourceSetting lineUrl=setting("url","URL", "text",remote.optString("url"));lineUrl.readOnly=true;
                SourceSetting lineCount=setting("count","App count","text",count<0?"Unknown":Integer.toString(count));lineCount.readOnly=true;
                SourceSetting lineRefresh=setting("refresh","Last refreshed","text",refreshed==0?"Never":java.text.DateFormat.getDateTimeInstance().format(new Date(refreshed*1000L)));lineRefresh.readOnly=true;
                group.children=new SourceSetting[]{enabled,lineUrl,lineCount,lineRefresh};result.add(group);
            }
            return result.toArray(new SourceSetting[0]);
        });}
        private SourceSetting setting(String id,String label,String type,String value){SourceSetting row=new SourceSetting();row.id=id;row.label=label;row.type=type;row.value=value;return row;}
        @Override public String setSetting(String id,String value){return call("setSetting id="+id,()->{synchronized(mutationLock){
            if("flathub_beta".equals(id)){
                boolean enabled=FlathubBetaPolicy.parseEnabled(value);
                List<String> installed=new ArrayList<>();if(!enabled){JSONArray apps=installedListings(false);for(int i=0;i<apps.length();i++){JSONObject app=apps.getJSONObject(i);if(app.optString("origin").equals(FlathubBetaPolicy.REMOTE))installed.add(app.optString("name",app.optString("id")));}}
                String error=FlathubBetaPolicy.setEnabled(enabled,installed,new FlathubBetaPolicy.RemoteActions(){
                    @Override public void enable() throws Exception {flatpak.cli("remote-add","--system","--if-not-exists","--from","--title","Flathub Beta",FlathubBetaPolicy.REMOTE,FlathubBetaPolicy.DESCRIPTOR);flatpak.cli("remote-modify","--system","--prio="+FlathubBetaPolicy.STABLE_PRIORITY,"flathub");flatpak.cli("remote-modify","--system","--prio="+FlathubBetaPolicy.BETA_PRIORITY,FlathubBetaPolicy.REMOTE);}
                    @Override public void remove() throws Exception {flatpak.cli("remote-delete","--system",FlathubBetaPolicy.REMOTE);}
                });if(error!=null)return error;
                invalidateCatalogue();invalidateUpdates();return null;
            }
            if(id!=null&&id.startsWith("remote-enabled:")){String remote=id.substring("remote-enabled:".length());validateRemote(remote);flatpak.cli("remote-modify","--system",Boolean.parseBoolean(value)?"--enable":"--disable",remote);invalidateCatalogue();invalidateUpdates();return null;}
            if(id!=null&&id.startsWith("repository:")&&value==null){String remote=id.substring("repository:".length());validateRemote(remote);if(remote.equals("flathub")||remote.equals("flatpark")||remote.equals("flathub-beta"))return "Built-in repository cannot be removed";flatpak.cli("remote-delete","--system",remote);invalidateCatalogue();invalidateUpdates();return null;}
            return "Unknown or read-only setting";
        }});}
        @Override public ActionResult runAction(String id,String[] values,boolean confirmed){return call("runAction id="+id,()->{ActionResult result=new ActionResult();if(!"add_repository".equals(id)){result.error="Unknown action";return result;}synchronized(mutationLock){try{String url=values.length>0?values[0].trim():"";String name=values.length>1?values[1].trim():"";URI uri=URI.create(url);if(!"https".equals(uri.getScheme())||uri.getHost()==null)throw new IllegalArgumentException("Repository descriptor requires an HTTPS host");String remote="store-"+java.util.HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(url.getBytes(StandardCharsets.UTF_8)));String title=name.isBlank()?uri.getHost():name;if(url.endsWith(".flatpakrepo"))flatpak.cli("remote-add","--system","--if-not-exists","--from","--title",title,remote,url);else flatpak.cli("remote-add","--system","--if-not-exists","--title",title,remote,url);invalidateCatalogue();invalidateUpdates();}catch(Exception error){result.error=error.getMessage();}return result;}});}
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
                    ensure();op.check();synchronized(mutationLock){StorePackages.install(FlatpakSourceService.this,flatpak,candidate,operation,terminal,op);}
                }catch(Exception error){Log.e(TAG,"install id="+id+" repo="+repo+" op="+operation+" failed",error);try{terminal.failed(operation,error.toString());}catch(RemoteException delivery){Log.e(TAG,"failed callback op="+operation,delivery);}}
                finally{invalidateUpdates();invalidateRepositoryMetadata();op.finish();if(began)endInstall(operation);else active.remove(operation);}
            },"flatpak-source-install");
            op.worker=worker;worker.start();
        }
        @Override public void installLocalFile(ParcelFileDescriptor file,String displayName,boolean allowUnsigned,String operation,IInstallCallback callback){
            if(file==null)throw new IllegalArgumentException("Local bundle descriptor required");
            if(callback==null){closeRejectedBundle(file,"Callback required");throw new IllegalArgumentException("Callback required");}
            if(displayName==null||displayName.isBlank()){closeRejectedBundle(file,"Display name required");throw new IllegalArgumentException("Display name required");}
            final int sourceFd=file.getFd();
            SourceOperations.Operation op;
            try{op=operations.start(operation);}catch(RuntimeException error){closeRejectedBundle(file,"Starting operation failed id="+operation);throw error;}
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
                    ensure();op.check();synchronized(mutationLock){StorePackages.installLocalFile(FlatpakSourceService.this,flatpak,file,displayName,allowUnsigned,operation,terminal,op);}
                }catch(Exception error){Log.e(TAG,"installLocalFile displayName="+displayName+" allowUnsigned="+allowUnsigned+" op="+operation+" failed",error);try{terminal.failed(operation,error.toString());}catch(RemoteException delivery){Log.e(TAG,"failed local-file callback op="+operation,delivery);}}
                finally{try{file.close();}catch(IOException error){Log.e(TAG,"close local bundle fd="+sourceFd+" at FlatpakSourceService.installLocalFile op="+operation+" failed",error);}invalidateUpdates();invalidateRepositoryMetadata();op.finish();if(began)endInstall(operation);else active.remove(operation);}
            },"flatpak-source-local-install");
            op.worker=worker;worker.start();
        }
        private void closeRejectedBundle(ParcelFileDescriptor file,String reason){int fd=file.getFd();try{file.close();}catch(IOException error){Log.e(TAG,"close rejected local bundle fd="+fd+" reason="+reason+" failed",error);}}

    };
    @Override public IBinder onBind(Intent intent){return binder;}
    private JSONArray repositories() throws Exception {
        JSONArray result=new JSONArray();
        for(String line:flatpak.cli("remotes","--system","--show-disabled","--columns=name,url,title,options").split("\n")) {
            if(line.isBlank())continue;String[] fields=line.split("\t",-1);if(fields.length==3)fields=new String[]{fields[0],fields[1],fields[2],""};if(fields.length!=4)throw new IOException("Invalid Flatpak remote row: "+line);
            String remote=fields[0];File active=new File(flatpak.installation,"appstream/"+remote+"/x86_64/active");File index=new File(active,"appstream.xml.gz");if(!index.isFile())index=new File(active,"appstream.xml");
            int count=-1;if(cachedCatalogue!=null){count=0;for(Map<String,Object> app:cachedCatalogue)if(remote.equals(app.get("origin")))count++;}
            long refreshed=index.isFile()?index.lastModified()/1000L:0;
            boolean enabled=!fields[3].toLowerCase(Locale.ROOT).contains("disabled");
            result.put(new JSONObject().put("id",remote).put("url",fields[1]).put("label",fields[2]).put("official",remote.equals("flathub")&&fields[1].equals("https://dl.flathub.org/repo/")).put("broken",brokenRemotes.getOrDefault(remote,false)).put("enabled",enabled).put("appCount",count).put("lastRefreshed",refreshed));
        }
        return result;
    }
    private List<Map<String,Object>> appStreamCatalogue() throws Exception {
        List<Map<String,Object>> cached=cachedCatalogue;if(cached!=null)return cached;
        CompletableFuture<List<Map<String,Object>>> pending;boolean load=false;
        synchronized(catalogueLock) {
            cached=cachedCatalogue;if(cached!=null)return cached;
            pending=catalogueLoad;
            if(pending==null){pending=new CompletableFuture<>();catalogueLoad=pending;load=true;}
        }
        if(load) {
            try {
                List<Map<String,Object>> result=loadCatalogue();
                synchronized(catalogueLock){if(catalogueLoad==pending)cachedCatalogue=result;}
                invalidateUpdates();
                pending.complete(result);
            } catch(Exception error) {
                pending.completeExceptionally(error);
                synchronized(catalogueLock){if(catalogueLoad==pending)catalogueLoad=null;}
                throw error;
            } catch(Error error) {
                pending.completeExceptionally(error);
                synchronized(catalogueLock){if(catalogueLoad==pending)catalogueLoad=null;}
                throw error;
            }
        }
        try { return pending.get(); }
        catch(InterruptedException error){Thread.currentThread().interrupt();throw new IOException("Interrupted while waiting for AppStream catalogue",error);}
        catch(ExecutionException error){Throwable cause=error.getCause();if(cause instanceof Exception)throw (Exception)cause;if(cause instanceof Error)throw (Error)cause;throw new IOException("AppStream catalogue load failed",cause);}
    }

    private List<Map<String,Object>> loadCatalogue() throws Exception {
        List<String> readableRemotes=new ArrayList<>();JSONArray remotes=repositories();
        for(int i=0;i<remotes.length();i++) {
            JSONObject repository=remotes.getJSONObject(i);if(!repository.optBoolean("enabled",true))continue;
            String remote=repository.getString("id");validateRemote(remote);readableRemotes.add(remote);
        }
        List<Map<String,Object>> result=RemoteAppStream.load(readableRemotes,brokenRemotes,
                remote->{try{flatpak.cli("update","--system","--appstream","--noninteractive",remote);}catch(Exception error){
                    Log.e(TAG,"AppStream refresh command failed remote="+remote+"; fetch stderr/cause follows",error);throw error;
                }},remote->{
            File active=new File(flatpak.installation,"appstream/"+remote+"/x86_64/active");File xml=new File(active,"appstream.xml.gz");
            boolean gzip=xml.isFile();if(!gzip)xml=new File(active,"appstream.xml");
            if(!xml.isFile())throw new IOException("AppStream XML missing remote="+remote+" active="+active);
            List<Map<String,Object>> rows=new ArrayList<>();
            try(InputStream input=new FileInputStream(xml)) {for(Map<String,Object> row:AppStreamCatalogue.read(input,gzip,remote)) {
                String icon=(String)row.get("icon");
                if(icon.startsWith("cached:"))row.put("icon",new android.net.Uri.Builder().scheme("content").authority("org.matonos.linux.store.icons")
                        .appendPath(remote).appendPath(icon.substring(7)).build().toString());
                row.put("id",row.get("ref"));
                row.put("badges",badges(row));
                row.put("packageName",org.matonos.linux.stub.DeployManifest.packageForPublic((String)row.get("ref")));rows.add(row);
            }}
            return rows;
        },(remote,stage,error)->Log.e(TAG,"AppStream "+stage+" failed remote="+remote+"; "+(stage.equals("refresh")?"checking cached XML":"skipping remote"),error));
        return result;
    }

    private void writeCatalogueRows(Writer output)throws Exception {
        JSONArray remoteRows=repositories();Map<String,String> labels=new HashMap<>();Map<String,Integer> priorities=new HashMap<>();List<String> enabled=new ArrayList<>();
        for(int i=0;i<remoteRows.length();i++){JSONObject remote=remoteRows.getJSONObject(i);String id=remote.getString("id");labels.put(id,remote.optString("label",id));if(remote.optBoolean("enabled",true))enabled.add(id);}
        try{for(String line:flatpak.cli("remote-list","--system","--columns=name,priority").split("\\n")){String[] fields=line.trim().split("\\s+");if(fields.length==2)priorities.put(fields[0],Integer.parseInt(fields[1]));}}catch(Exception error){Log.e(TAG,"Reading repository priorities failed; using source order",error);for(int i=0;i<enabled.size();i++)priorities.put(enabled.get(i),enabled.size()-i);}
        if(appStreamFetchDue||addedCache==null){addedCache=addedByAppId();popularityCache=popularityByAppId();}Map<String,Long> added=addedCache;Map<String,Double> scores=popularityCache;Set<String> installed=flatpak.installedRefs(false),updates=flatpak.installedRefs(true);
        boolean[] first={true};int[] succeeded={0};Exception[] last={null};
        for(String remote:enabled){
            File active=new File(flatpak.installation,"appstream/"+remote+"/x86_64/active"),xml=new File(active,"appstream.xml.gz");boolean gzip=xml.isFile();if(!gzip)xml=new File(active,"appstream.xml");
            boolean refreshFailed=false,fetched=appStreamFetchDue||!xml.isFile();if(fetched){try{flatpak.cli("update","--system","--appstream","--noninteractive",remote);}catch(Exception error){refreshFailed=true;brokenRemotes.put(remote,true);Log.e(TAG,"AppStream refresh failed remote="+remote+"; reading cached index",error);}
            xml=new File(active,"appstream.xml.gz");gzip=xml.isFile();if(!gzip)xml=new File(active,"appstream.xml");}
            if(!xml.isFile()){Log.e(TAG,"AppStream index missing remote="+remote+" path="+active);continue;}
            try(InputStream input=new FileInputStream(xml)){
                AppStreamCatalogue.readEach(input,gzip,remote,row->{
                    String ref=String.valueOf(row.get("ref"));row.put("id",ref);row.put("repo",remote);row.put("repoLabel",labels.get(remote));row.put("repoPriority",priorities.getOrDefault(remote,0));
                    String icon=String.valueOf(row.get("icon"));if(icon.startsWith("cached:"))row.put("icon",new android.net.Uri.Builder().scheme("content").authority("org.matonos.linux.store.icons").appendPath(remote).appendPath(icon.substring(7)).build().toString());
                    row.put("packageName",org.matonos.linux.stub.DeployManifest.packageForPublic(ref));
                    String[] parts=ref.split("/");String appId=parts.length>1?parts[1]:ref;Long addedAt=added.get(appId);if(addedAt!=null)row.put("added",addedAt);
                    row.put("badges",badges(row));row.put("popularityScore",scores.getOrDefault(appId,0.0));row.put("installed",installed.contains(ref));row.put("updateAvailable",updates.contains(ref));row.put("currentVersionName",JSONObject.NULL);row.put("installable",true);row.put("signerMatches",true);
                    if(!first[0])output.write(',');output.write(catalogueRow(row).toString());first[0]=false;succeeded[0]++;
                });
                if(fetched)brokenRemotes.put(remote,refreshFailed);
            }catch(Exception error){brokenRemotes.put(remote,true);last[0]=error;Log.e(TAG,"Reading AppStream catalogue failed remote="+remote,error);}
        }
        if(succeeded[0]==0&&last[0]!=null)throw last[0];
    }
    private Map<String,Long> addedByAppId(){
        Map<String,Long> result=new HashMap<>();try{JSONArray hits=getJson("https://flathub.org/api/v2/collection/recently-added").getJSONArray("hits");for(int i=0;i<hits.length();i++){JSONObject hit=hits.getJSONObject(i);result.put(hit.optString("app_id"),SourceTypes.epochSeconds(hit.opt("added"),hit.opt("timestamp"),hit.opt("date")));}}catch(Exception error){Log.e(TAG,"Fetching Flathub New dates failed",error);}return result;
    }
    private Map<String,Double> popularityByAppId(){
        Map<String,Double> result=new HashMap<>();try{JSONArray hits=getJson("https://flathub.org/api/v2/collection/popular").getJSONArray("hits");long max=0;for(int i=0;i<hits.length();i++){JSONObject hit=hits.getJSONObject(i);max=Math.max(max,Math.max(0,hit.optLong("installs_last_month",hit.optLong("installsLastMonth",hit.optLong("installs",0)))));}double denominator=Math.log1p(max);for(int i=0;i<hits.length();i++){JSONObject hit=hits.getJSONObject(i);long count=Math.max(0,hit.optLong("installs_last_month",hit.optLong("installsLastMonth",hit.optLong("installs",0))));result.put(hit.optString("app_id"),denominator>0?Math.round(10.0*Math.log1p(count)/denominator*10.0)/10.0:0.0);}}catch(Exception error){Log.e(TAG,"Fetching Flathub popularity failed",error);}return result;
    }

    private JSONObject catalogueRow(Map<String,Object> row)throws Exception{
        JSONObject value=new JSONObject();
        for(String key:new String[]{"id","name","summary","icon","developer","installed","repo","versionName","packageName","added","badges","repoOptions"})if(row.containsKey(key))value.put(key,JSONObject.wrap(row.get(key)));
        value.put("repo",row.getOrDefault("repo",row.get("origin")));value.put("repoLabel",row.getOrDefault("repoLabel",row.getOrDefault("originTitle",row.get("origin"))));
        value.put("popularityScore",row.get("popularityScore") instanceof Number?((Number)row.get("popularityScore")).doubleValue():0.0);
        value.put("currentVersionName",row.get("currentVersionName")==null?JSONObject.NULL:row.get("currentVersionName"));value.put("repoPriority",row.get("repoPriority") instanceof Number?((Number)row.get("repoPriority")).intValue():0);value.put("signerMatches",true);value.put("installable",true);value.put("updateAvailable",Boolean.TRUE.equals(row.get("updateAvailable")));
        return value;
    }

    /** A search is a local filter only; it never refreshes AppStream or performs network decoration. */
    private void invalidateCatalogue() {
        long version=catalogueGeneration.incrementAndGet();getSharedPreferences("store-catalogue",MODE_PRIVATE).edit().putLong("version",version).apply();
        synchronized(catalogueLock){cachedCatalogue=null;catalogueLoad=null;}
        invalidateRepositoryMetadata();
    }

    private List<Map<String,Object>> withRepositoryOptions(List<Map<String,Object>> rows) throws Exception {
        if(rows.isEmpty())return rows;
        Map<String,List<Map<String,Object>>> byId=new HashMap<>();
        for(Map<String,Object> candidate:appStreamCatalogue())byId.computeIfAbsent(String.valueOf(candidate.get("id")),key->new ArrayList<>()).add(candidate);
        RepositoryMetadata metadata=repositoryMetadata();
        Map<String,String> names=metadata.names;Map<String,Integer> order=metadata.order;
        Map<String,String> installedOrigins=metadata.installedOrigins;Map<String,Integer> priorities=metadata.priorities;
        for(Map<String,Object> row:rows){String id=String.valueOf(row.get("id"));List<Map<String,Object>> candidates=byId.getOrDefault(id,Collections.singletonList(row));
            candidates=new ArrayList<>(candidates);candidates.sort((a,b)->{String ar=String.valueOf(a.get("origin")),br=String.valueOf(b.get("origin"));int ap=priorities.getOrDefault(ar,Integer.MIN_VALUE),bp=priorities.getOrDefault(br,Integer.MIN_VALUE);if(ap!=bp)return Integer.compare(bp,ap);return Integer.compare(order.getOrDefault(ar,Integer.MAX_VALUE),order.getOrDefault(br,Integer.MAX_VALUE));});
            String installedOrigin=installedOrigins.get(id);if(installedOrigin==null&&Boolean.TRUE.equals(row.get("installed")))installedOrigin=String.valueOf(row.get("origin"));
            if(installedOrigin!=null){row.put("installed",true);row.put("origin",installedOrigin);}
            List<Map<String,Object>> options=new ArrayList<>();Set<String> seen=new HashSet<>();
            for(Map<String,Object> candidate:candidates){String remote=String.valueOf(candidate.get("origin"));if(!seen.add(remote))continue;boolean installable=installedOrigin==null||installedOrigin.equals(remote);Map<String,Object> option=new LinkedHashMap<>();option.put("repoId",remote);option.put("name",names.getOrDefault(remote,remote));option.put("installable",installable);if(!installable)option.put("note","Reinstall from this repository");options.add(option);}
            if(installedOrigin!=null&&seen.add(installedOrigin)){Map<String,Object> option=new LinkedHashMap<>();option.put("repoId",installedOrigin);option.put("name",names.getOrDefault(installedOrigin,installedOrigin));option.put("installable",false);option.put("note","Installed repository is unavailable");options.add(option);}
            row.put("repoOptions",options);
        }
        return rows;
    }
    private void addVerifiedBadge(Map<String,Object> row) {
        if(!"flathub".equals(row.get("origin")))return;
        String ref=String.valueOf(row.getOrDefault("ref",row.get("id")));String[] parts=ref.split("/");if(parts.length<2)return;String appId=parts[1];Boolean verified=verifiedApps.get(appId);
        if(verified==null)try {JSONObject result=getJson("https://flathub.org/api/v2/verification/"+URLEncoder.encode(appId,StandardCharsets.UTF_8)+"/status");verified=result.optBoolean("verified",false)||"verified".equalsIgnoreCase(result.optString("status"));verifiedApps.put(appId,verified);}
        catch(Exception error){Log.e(TAG,"Flathub verification lookup app="+appId+" failed",error);verifiedApps.put(appId,false);verified=false;}
        if(verified){@SuppressWarnings("unchecked") List<Map<String,Object>> badges=(List<Map<String,Object>>)row.computeIfAbsent("badges",key->new ArrayList<>());badges.add(Map.of("id","verified","label","Verified developer","icon","verified"));}
    }
    private List<Map<String,Object>> badges(Map<String,Object> row) {
        List<Map<String,Object>> badges=new ArrayList<>();
        String license=String.valueOf(row.getOrDefault("license",""));
        if(license.matches("(?i).*(GPL|LGPL|AGPL|MIT|Apache|BSD|MPL|CC0|ISC|Zlib).*"))badges.add(Map.of("id","open-source","label","Open source","icon","code"));
        Object added=row.get("added");if(added instanceof Number&&((Number)added).longValue()>System.currentTimeMillis()/1000L-30L*86400)badges.add(Map.of("id","new","label","New","icon","fiber_new"));
        return badges;
    }
    private JSONObject getJson(String address) throws Exception {
        HttpURLConnection connection=(HttpURLConnection)new URL(address).openConnection();
        try {connection.setConnectTimeout(15000);connection.setReadTimeout(30000);
            if(connection.getResponseCode()!=200)throw new IOException("HTTP "+connection.getResponseCode()+" url="+address);
            try(InputStream in=connection.getInputStream()){return new JSONObject(new String(in.readAllBytes(),StandardCharsets.UTF_8));}
        } finally {connection.disconnect();}
    }
    private Map<String,Object> find(String id) throws Exception {return find(id,null);}
    private Map<String,Object> find(String id,String repo) throws Exception {
        return new LinkedHashMap<>(SourceCandidates.find(appStreamCatalogue(),id,repo));
    }

    private void invalidateUpdates() { long version=catalogueGeneration.incrementAndGet();getSharedPreferences("store-catalogue",MODE_PRIVATE).edit().putLong("version",version).apply(); }
    private static long elapsedMs(long start,long end) { return (end-start)/1_000_000L; }
    private static final class RepositoryMetadata {
        final Map<String,String> names;final Map<String,Integer> order,priorities;final Map<String,String> installedOrigins;
        RepositoryMetadata(Map<String,String> names,Map<String,Integer> order,Map<String,Integer> priorities,Map<String,String> installedOrigins) {
            this.names=Collections.unmodifiableMap(names);this.order=Collections.unmodifiableMap(order);
            this.priorities=Collections.unmodifiableMap(priorities);this.installedOrigins=Collections.unmodifiableMap(installedOrigins);
        }
    }
    private RepositoryMetadata repositoryMetadata() throws Exception {
        RepositoryMetadata cached=cachedRepositoryMetadata;if(cached!=null)return cached;
        synchronized(repositoryMetadataLock) {
            cached=cachedRepositoryMetadata;if(cached!=null)return cached;
            long started=System.nanoTime();Map<String,String> names=new HashMap<>();Map<String,Integer> order=new HashMap<>();
            JSONArray remotes=repositories();for(int i=0;i<remotes.length();i++){JSONObject remote=remotes.getJSONObject(i);String id=remote.getString("id");names.put(id,remote.optString("label",id));order.put(id,i);}
            long remotesDone=System.nanoTime();Map<String,Integer> priorities=new HashMap<>();
            try {for(String line:flatpak.cli("remote-list","--system","--columns=name,priority").split("\\n")){String[] fields=line.trim().split("\\s+");if(fields.length==2)priorities.put(fields[0],Integer.parseInt(fields[1]));}}
            catch(Exception error){Log.w(TAG,"Reading Flatpak remote priorities failed; using backend remote order",error);}
            long prioritiesDone=System.nanoTime();Map<String,String> installedOrigins=new HashMap<>();
            JSONArray installedRows=installedListings(false);for(int i=0;i<installedRows.length();i++){JSONObject installed=installedRows.getJSONObject(i);installedOrigins.put(installed.optString("id"),installed.optString("origin"));}
            long installedDone=System.nanoTime();cached=new RepositoryMetadata(names,order,priorities,installedOrigins);cachedRepositoryMetadata=cached;
            Log.i(TAG,"repository metadata refresh remotes_ms="+elapsedMs(started,remotesDone)+" priorities_ms="+elapsedMs(remotesDone,prioritiesDone)+" installed_origins_ms="+elapsedMs(prioritiesDone,installedDone));
            return cached;
        }
    }
    private void invalidateRepositoryMetadata() { synchronized(repositoryMetadataLock){cachedRepositoryMetadata=null;} }
    private JSONArray installedListings(boolean updates) throws Exception {
        Set<String> selected=flatpak.installedRefs(updates);
        List<Map<String,Object>> catalogue;
        if(updates) {
            catalogue=cachedCatalogue;
            if(catalogue==null)catalogue=Collections.emptyList();
        } else try{catalogue=appStreamCatalogue();}catch(Exception error){
            Log.e(TAG,"Cannot decorate installed refs with AppStream; retaining signed installed listings",error);catalogue=Collections.emptyList();
        }
        JSONArray result=new JSONArray();
        for(PackageInfo pkg:getPackageManager().getInstalledPackages(PackageManager.GET_SIGNING_CERTIFICATES)) {
            if(!trusted(pkg))continue;
            List<DeploymentRecord> records;
            try{records=DeploymentRecord.installedAll(pkg.splitNames,pkg.applicationInfo.splitSourceDirs);}
            catch(IOException incomplete){Log.w(TAG,"Incomplete installed carrier package="+pkg.packageName,incomplete);continue;}
            for(DeploymentRecord record:records) {
                String expectedPackage=org.matonos.linux.stub.DeployManifest.packageForPublic(record.ref);
                if(!pkg.packageName.equals(expectedPackage)) {
                    Log.e(TAG,"Installed carrier package/ref mismatch package="+pkg.packageName+" ref="+record.ref+" expected="+expectedPackage);
                    continue;
                }
                String[] parts=record.ref.split("/");if(!parts[0].equals("app"))continue;
                RemoteOrigin origin=RemoteOrigin.parse(record.remote);
                if(!selected.contains(record.ref))continue;
                Map<String,Object> listing=new LinkedHashMap<>();for(Map<String,Object> row:catalogue)if(record.ref.equals(row.get("ref"))&&origin.name.equals(row.get("origin"))){listing.putAll(row);break;}
                listing.put("id",record.ref);listing.put("name",listing.getOrDefault("name",parts[1]));listing.put("origin",origin.name);listing.put("packageName",expectedPackage);listing.put("installed",true);
                Map<String,Object> visible=AppStreamCatalogue.search(Collections.singletonList(listing),"").get(0);
                result.put(new JSONObject(visible));
            }
        }
        return result;
    }
    public static boolean trusted(PackageInfo pkg) throws Exception {
        if(pkg.packageName==null||!pkg.packageName.startsWith("flatpak.")||pkg.signingInfo==null||pkg.signingInfo.hasMultipleSigners())return false;
        Signature[] signatures=pkg.signingInfo.getApkContentsSigners();byte[] expected=StubGenerator.getExistingSigningCertificate();
        return expected!=null&&signatures.length==1&&MessageDigest.isEqual(expected,signatures[0].toByteArray());
    }
    private static void validateRemote(String remote) throws IOException {if(remote==null||!remote.matches("[A-Za-z0-9_.-]+")||remote.startsWith("-"))throw new IOException("Invalid remote name: "+remote);}
}
