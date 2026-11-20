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
    private final Object catalogueLock=new Object();
    private CompletableFuture<List<Map<String,Object>>> catalogueLoad;
    private final Object mutationLock=new Object();
    private final Object updateLock=new Object();
    private volatile AppListing[] cachedUpdates;
    private volatile long cachedUpdatesAt;
    private static final long UPDATE_CACHE_MILLIS=60_000L;
    private final Map<String,List<Map<String,Object>>> cachedCollections=new HashMap<>();
    private Map<String, JSONObject> cachedRatings;
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
        packageReceiver=new BroadcastReceiver() {
            @Override public void onReceive(android.content.Context context,Intent intent) { invalidateUpdates(); }
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
        @Override public SourceInfo info(){SourceInfo info=new SourceInfo();info.version=ISoftwareSource.VERSION;info.revision=ISoftwareSource.REVISION;info.name="flatpak";info.label="Flatpak";info.description="Desktop applications from configured software repositories.";info.capabilities=CAP_AGE_RATINGS|CAP_REVIEWS|CAP_PERMISSIONS;return info;}
        @Override public AppListing[] search(String term,int offset,int limit){return call("search term="+term,()->{List<Map<String,Object>> page=copyRows(SourcePaging.page(AppStreamCatalogue.search(catalogue(),term),offset,limit));return SourceTypes.listings(withRepositoryOptions(withRatings(page)),0,page.size());});}
        @Override public AppListing[] collection(String kind,int offset,int limit){return call("collection kind="+kind,()->SourceTypes.listings(withRepositoryOptions(collectionListings(kind)),offset,limit));}
        @Override public PopularApp[] getPopularApps(int limit){return call("popular apps limit="+limit,()->popularApps(limit));}
        @Override public AppDetails details(String id,String repo){return call("details id="+id,()->{
            Map<String,Object> row=find(id,repo);row=withRepositoryOptions(Collections.singletonList(row)).get(0);String remote=(String)row.get("origin"),ref=(String)row.get("ref");
            String appId=id.startsWith("app/")&&id.split("/").length>1?id.split("/")[1]:id;
            JSONObject ratingAggregate=ratingFor(appId);if(ratingAggregate!=null)row.put("rating",ratingAggregate);
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
        @Override public Review[] reviews(String id,int offset,int limit){return call("reviews id="+id,()->SourceTypes.reviews(new JSONArray(odrs(id)),offset,limit));}
        @Override public AppListing[] installed(int offset,int limit){return call("installed",()->SourceTypes.listings(withRepositoryOptions(installedListings(false)),offset,limit));}
        @Override public AppListing[] updates(int offset,int limit){return call("updates",()->{
            AppListing[] rows=cachedUpdates;
            if(rows==null||System.currentTimeMillis()-cachedUpdatesAt>UPDATE_CACHE_MILLIS) synchronized(updateLock) {
                rows=cachedUpdates;
                if(rows==null||System.currentTimeMillis()-cachedUpdatesAt>UPDATE_CACHE_MILLIS) {
                    rows=SourceTypes.listings(installedListings(true),0,Integer.MAX_VALUE);
                    cachedUpdates=rows;cachedUpdatesAt=System.currentTimeMillis();
                }
            }
            return page(rows,offset,limit);
        });}
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
                invalidateCatalogue();invalidateUpdates();synchronized(cachedCollections){cachedCollections.clear();}return null;
            }
            if(id!=null&&id.startsWith("remote-enabled:")){String remote=id.substring("remote-enabled:".length());validateRemote(remote);flatpak.cli("remote-modify","--system",Boolean.parseBoolean(value)?"--enable":"--disable",remote);invalidateCatalogue();invalidateUpdates();synchronized(cachedCollections){cachedCollections.clear();}return null;}
            if(id!=null&&id.startsWith("repository:")&&value==null){String remote=id.substring("repository:".length());validateRemote(remote);if(remote.equals("flathub")||remote.equals("flatpark")||remote.equals("flathub-beta"))return "Built-in repository cannot be removed";flatpak.cli("remote-delete","--system",remote);invalidateCatalogue();invalidateUpdates();synchronized(cachedCollections){cachedCollections.clear();}return null;}
            return "Unknown or read-only setting";
        }});}
        @Override public ActionResult runAction(String id,String[] values,boolean confirmed){return call("runAction id="+id,()->{ActionResult result=new ActionResult();if(!"add_repository".equals(id)){result.error="Unknown action";return result;}synchronized(mutationLock){try{String url=values.length>0?values[0].trim():"";String name=values.length>1?values[1].trim():"";URI uri=URI.create(url);if(!"https".equals(uri.getScheme())||uri.getHost()==null)throw new IllegalArgumentException("Repository descriptor requires an HTTPS host");String remote="store-"+java.util.HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(url.getBytes(StandardCharsets.UTF_8)));String title=name.isBlank()?uri.getHost():name;if(url.endsWith(".flatpakrepo"))flatpak.cli("remote-add","--system","--if-not-exists","--from","--title",title,remote,url);else flatpak.cli("remote-add","--system","--if-not-exists","--title",title,remote,url);invalidateCatalogue();invalidateUpdates();synchronized(cachedCollections){cachedCollections.clear();}}catch(Exception error){result.error=error.getMessage();}return result;}});}
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
                finally{invalidateUpdates();op.finish();if(began)endInstall(operation);else active.remove(operation);}
            },"flatpak-source-install");
            op.worker=worker;worker.start();
        }

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
    private List<Map<String,Object>> catalogue() throws Exception {
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

    /** A search is a local filter only; it never refreshes AppStream or performs network decoration. */
    private void invalidateCatalogue() {
        synchronized(catalogueLock){cachedCatalogue=null;catalogueLoad=null;}
    }

    private JSONArray withRepositoryOptions(JSONArray rows) throws Exception {
        List<Map<String,Object>> values=new ArrayList<>();
        for(int i=0;i<rows.length();i++){JSONObject object=rows.getJSONObject(i);Map<String,Object> value=new LinkedHashMap<>();Iterator<String> keys=object.keys();while(keys.hasNext()){String key=keys.next();value.put(key,object.get(key));}values.add(value);}
        JSONArray result=new JSONArray();for(Map<String,Object> value:withRepositoryOptions(values))result.put(new JSONObject(value));return result;
    }

    private static List<Map<String,Object>> copyRows(List<Map<String,Object>> rows) {
        List<Map<String,Object>> result=new ArrayList<>(rows.size());
        for(Map<String,Object> row:rows)result.add(new LinkedHashMap<>(row));
        return result;
    }

    private List<Map<String,Object>> withRepositoryOptions(List<Map<String,Object>> rows) throws Exception {
        if(rows.isEmpty())return rows;
        Map<String,List<Map<String,Object>>> byId=new HashMap<>();
        for(Map<String,Object> candidate:catalogue())byId.computeIfAbsent(String.valueOf(candidate.get("id")),key->new ArrayList<>()).add(candidate);
        JSONArray remotes=repositories();Map<String,String> names=new HashMap<>();Map<String,Integer> order=new HashMap<>();
        for(int i=0;i<remotes.length();i++){JSONObject remote=remotes.getJSONObject(i);String id=remote.getString("id");names.put(id,remote.optString("label",id));order.put(id,i);}
        Map<String,String> installedOrigins=new HashMap<>();JSONArray installedRows=installedListings(false);for(int i=0;i<installedRows.length();i++){JSONObject installed=installedRows.getJSONObject(i);installedOrigins.put(installed.optString("id"),installed.optString("origin"));}
        Map<String,Integer> priorities=new HashMap<>();
        try {for(String line:flatpak.cli("remote-list","--system","--columns=name,priority").split("\\n")){String[] fields=line.trim().split("\\s+");if(fields.length==2)priorities.put(fields[0],Integer.parseInt(fields[1]));}}
        catch(Exception error){Log.w(TAG,"Reading Flatpak remote priorities failed; using backend remote order",error);}
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
    private List<Map<String,Object>> collectionListings(String kind) throws Exception {
        if(!"popular".equals(kind)&&!"new".equals(kind))return Collections.emptyList();
        synchronized(cachedCollections){List<Map<String,Object>> cached=cachedCollections.get(kind);if(cached!=null)return copyRows(cached);}
        String collection="new".equals(kind)?"recently-added":"popular";
        JSONArray response=getJson("https://flathub.org/api/v2/collection/"+collection).getJSONArray("hits");
        Map<String,Map<String,Object>> appsById=new HashMap<>();
        for(Map<String,Object> row:catalogue()) {
            String ref=(String)row.get("id"),remote=(String)row.get("origin");
            if(ref==null||!ref.startsWith("app/")||remote==null)continue;
            if(!"flathub".equals(remote))continue;
            String[] parts=ref.split("/");if(parts.length==4)appsById.putIfAbsent(parts[1],row);
        }
        List<Map<String,Object>> result=new ArrayList<>();
        for(int i=0;i<response.length();i++) {
            JSONObject hit=response.getJSONObject(i);Map<String,Object> row=appsById.get(hit.optString("app_id"));
            if(row==null)continue;
            Map<String,Object> listing=AppStreamCatalogue.search(Collections.singletonList(row),"").get(0);
            listing.put("added",SourceTypes.epochSeconds(hit.opt("added"),hit.opt("timestamp"),hit.opt("date")));
            JSONObject rating=ratingFor(hit.optString("app_id"));if(rating!=null)listing.put("rating",rating);
            result.add(listing);
        }
        synchronized(cachedCollections){cachedCollections.put(kind,copyRows(result));}return copyRows(result);
    }
    private PopularApp[] popularApps(int limit) throws Exception {
        if(limit<=0)throw new IllegalArgumentException("limit must be positive");
        JSONArray hits=getJson("https://flathub.org/api/v2/collection/popular").getJSONArray("hits");
        Map<String,Map<String,Object>> appsById=new HashMap<>();
        for(Map<String,Object> row:catalogue()) {
            String ref=(String)row.get("id"),remote=(String)row.get("origin");
            if(ref==null||!ref.startsWith("app/")||!"flathub".equals(remote))continue;
            String[] parts=ref.split("/");if(parts.length==4)appsById.putIfAbsent(parts[1],row);
        }
        List<JSONObject> accepted=new ArrayList<>();long maximum=0;
        for(int i=0;i<hits.length();i++) {
            JSONObject hit=hits.getJSONObject(i);Map<String,Object> row=appsById.get(hit.optString("app_id"));
            if(row==null)continue;
            long installs=Math.max(0,hit.optLong("installs_last_month",hit.optLong("installsLastMonth",hit.optLong("installs",0))));
            hit.put("_matonos_installs",installs);maximum=Math.max(maximum,installs);accepted.add(hit);
        }
        double denominator=Math.log1p(maximum);List<Map<String,Object>> listings=new ArrayList<>();List<Double> scores=new ArrayList<>();
        for(int i=0;i<accepted.size()&&listings.size()<limit;i++) {
            JSONObject hit=accepted.get(i);Map<String,Object> row=appsById.get(hit.optString("app_id"));
            Map<String,Object> listing=AppStreamCatalogue.search(Collections.singletonList(row),"").get(0);
            JSONObject rating=ratingFor(hit.optString("app_id"));if(rating!=null)listing.put("rating",rating);
            long installs=hit.optLong("_matonos_installs");
            double score=denominator>0?10.0*Math.log1p(installs)/denominator:10.0*(accepted.size()-i)/(double)Math.max(1,accepted.size());
            listing.put("badges",row.get("badges"));listings.add(listing);scores.add(Math.round(score*10.0)/10.0);
        }
        List<PopularApp> result=new ArrayList<>();listings=withRepositoryOptions(listings);
        for(int i=0;i<listings.size();i++){PopularApp item=new PopularApp();item.app=SourceTypes.listings(Collections.singletonList(listings.get(i)),0,1)[0];item.score=scores.get(i);result.add(item);}
        return result.toArray(new PopularApp[0]);
    }
    private List<Map<String,Object>> withRatings(List<Map<String,Object>> rows) {
        for(Map<String,Object> row:rows) {
            Object rawId=row.get("id");if(!(rawId instanceof String))continue;
            String id=(String)rawId;if(id.startsWith("app/")){String[] parts=id.split("/");if(parts.length>1)id=parts[1];}
            JSONObject rating=ratingFor(id);if(rating!=null)row.put("rating",rating);
        }
        // Verification is one HTTP request per Flathub app: fetch the page's lookups in parallel.
        rows.parallelStream().forEach(this::addVerifiedBadge);
        return rows;
    }
    private JSONObject getJson(String address) throws Exception {
        HttpURLConnection connection=(HttpURLConnection)new URL(address).openConnection();
        try {connection.setConnectTimeout(15000);connection.setReadTimeout(30000);
            if(connection.getResponseCode()!=200)throw new IOException("HTTP "+connection.getResponseCode()+" url="+address);
            try(InputStream in=connection.getInputStream()){return new JSONObject(new String(in.readAllBytes(),StandardCharsets.UTF_8));}
        } finally {connection.disconnect();}
    }
    private synchronized JSONObject ratingFor(String id) {
        try {
            if(cachedRatings==null) {
                JSONObject raw=getJson("https://odrs.gnome.org/1.0/reviews/api/ratings");
                cachedRatings=new HashMap<>();java.util.Iterator<String> keys=raw.keys();while(keys.hasNext()){String key=keys.next();cachedRatings.put(key,raw.getJSONObject(key));}
            }
            JSONObject raw=cachedRatings.get(id);if(raw==null)raw=cachedRatings.get(id+".desktop");if(raw==null)return null;
            int[] histogram={raw.optInt("star1"),raw.optInt("star2"),raw.optInt("star3"),raw.optInt("star4"),raw.optInt("star5")};
            int count=0;long sum=0;for(int i=0;i<histogram.length;i++){count+=histogram[i];sum+=(long)histogram[i]*(i+1);}
            if(count==0)return null;Rating result=new Rating();result.average=(float)Math.round((sum/(double)count)*10.0)/10.0f;result.count=count;result.histogram=histogram;
            JSONObject out=new JSONObject().put("average",result.average).put("count",result.count).put("histogram",new JSONArray(result.histogram));return out;
        } catch(Exception error) {Log.e(TAG,"ODRS rating lookup id="+id+" failed",error);cachedRatings=Collections.emptyMap();return null;}
    }
    private Map<String,Object> find(String id) throws Exception {return find(id,null);}
    private Map<String,Object> find(String id,String repo) throws Exception {
        return new LinkedHashMap<>(SourceCandidates.find(catalogue(),id,repo));
    }

    private void invalidateUpdates() { cachedUpdates=null;cachedUpdatesAt=0; }
    private static AppListing[] page(AppListing[] rows,int offset,int limit) {
        if(offset<0||limit<=0)throw new IllegalArgumentException("offset must be non-negative and limit positive");
        if(offset>=rows.length)return new AppListing[0];
        return Arrays.copyOfRange(rows,offset,(int)Math.min((long)rows.length,(long)offset+limit));
    }
    private JSONArray installedListings(boolean updates) throws Exception {
        Set<String> selected=flatpak.installedRefs(updates);
        List<Map<String,Object>> catalogue;
        if(updates) {
            catalogue=cachedCatalogue;
            if(catalogue==null)catalogue=Collections.emptyList();
        } else try{catalogue=catalogue();}catch(Exception error){
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
            Map<String,Object> visible=AppStreamCatalogue.search(Collections.singletonList(listing),"").get(0);
            if(!updates){JSONObject rating=ratingFor(parts[1]);if(rating!=null)visible.put("rating",rating);}
            result.put(new JSONObject(visible));
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
