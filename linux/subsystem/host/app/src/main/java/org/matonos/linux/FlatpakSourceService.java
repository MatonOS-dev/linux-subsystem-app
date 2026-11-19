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

/** Software Centre source; installation is committed by the bound store. */
public final class FlatpakSourceService extends Service {
    private static final String TAG="FlatpakSource";
    private StoreFlatpak flatpak;
    private final SourceOperations operations=new SourceOperations();
    private List<Map<String,Object>> cachedCatalogue;
    private final Map<String,List<Map<String,Object>>> cachedCollections=new HashMap<>();
    private Map<String, JSONObject> cachedRatings;
    private final Map<String,Boolean> verifiedApps=new HashMap<>();
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
        seedDefaultRemotes(candidate);
        flatpak=candidate;
    }
    private void seedDefaultRemotes(StoreFlatpak candidate) {
        File marker=new File(candidate.root,"default-remotes-seeded");
        if(marker.exists())return;
        String[][] defaults={{"flathub","https://dl.flathub.org/repo/flathub.flatpakrepo"},{"flatpark","https://dl.flatpark.org/flatpark.flatpakrepo"}};
        for(String[] remote:defaults)try {
            candidate.cli("remote-add","--system","--if-not-exists","--from",remote[0],remote[1]);
        } catch(Exception error) { Log.e(TAG,"Seeding default remote="+remote[0]+" url="+remote[1]+" failed",error); }
        try { Files.write(marker.toPath(),new byte[]{49}); }
        catch(IOException error) { Log.e(TAG,"Recording default remote seed state failed",error); }
    }
    private final ISoftwareSource.Stub binder=new ISoftwareSource.Stub() {
        @Override public SourceInfo info(){SourceInfo info=new SourceInfo();info.version=ISoftwareSource.VERSION;info.revision=ISoftwareSource.REVISION;info.name="flatpak";info.label="Linux apps";info.description="Desktop applications from configured software repositories.";info.capabilities=CAP_REPOSITORIES|CAP_AGE_RATINGS|CAP_REVIEWS|CAP_PERMISSIONS;return info;}
        @Override public AppListing[] search(String term,int offset,int limit){return call("search term="+term,()->SourceTypes.listings(withRatings(AppStreamCatalogue.search(catalogue(),term)),offset,limit));}
        @Override public AppListing[] collection(String kind,int offset,int limit){return call("collection kind="+kind,()->SourceTypes.listings(collectionListings(kind),offset,limit));}
        @Override public PopularApp[] getPopularApps(int limit){return call("popular apps limit="+limit,()->popularApps(limit));}
        @Override public AppDetails details(String id,String repo){return call("details id="+id,()->{
            Map<String,Object> row=find(id,repo);String remote=(String)row.get("origin"),ref=(String)row.get("ref");
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
        @Override public AppListing[] installed(int offset,int limit){return call("installed",()->SourceTypes.listings(installedListings(false),offset,limit));}
        @Override public AppListing[] updates(int offset,int limit){return call("updates",()->SourceTypes.listings(installedListings(true),offset,limit));}
        @Override public Repo[] repos(int offset,int limit){return call("repos",()->SourceTypes.repos(repositories(),offset,limit));}
        @Override public void setRepoEnabled(String id,boolean enabled){call("setRepoEnabled id="+id+" enabled="+enabled,()->{validateRemote(id);flatpak.cli("remote-modify","--system",enabled?"--enable":"--disable",id);cachedCatalogue=null;synchronized(cachedCollections){cachedCollections.clear();}return "";});}
        @Override public void addRepo(String url){addRepoNamed("",url);}
        @Override public void addRepoNamed(String label,String url){call("addRepo url="+url,()->{
            URI uri=URI.create(url);if(!"https".equals(uri.getScheme())||uri.getHost()==null)throw new IOException("Repository descriptor requires an HTTPS host");
            String name="store-"+java.util.HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(url.getBytes(StandardCharsets.UTF_8)));
            String title=label==null||label.isBlank()?uri.getHost():label.trim();
            flatpak.cli("remote-add","--system","--if-not-exists","--title",title,name,url);cachedCatalogue=null;synchronized(cachedCollections){cachedCollections.clear();}return "";
        });}
        @Override public void delRepo(String id){call("delRepo id="+id,()->{validateRemote(id);if(id.equals("flathub")||id.equals("flatpark"))throw new IllegalArgumentException("Built-in repository cannot be removed");flatpak.cli("remote-delete","--system",id);cachedCatalogue=null;synchronized(cachedCollections){cachedCollections.clear();}return "";});}
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
        for(String line:flatpak.cli("remotes","--system","--show-disabled","--columns=name,url,title,options").split("\n")) {
            if(line.isBlank())continue;String[] fields=line.split("\t",-1);if(fields.length!=4)throw new IOException("Invalid Flatpak remote row: "+line);
            String remote=fields[0];File active=new File(flatpak.installation,"appstream/"+remote+"/x86_64/active");File index=new File(active,"appstream.xml.gz");if(!index.isFile())index=new File(active,"appstream.xml");
            int count=-1;if(cachedCatalogue!=null){count=0;for(Map<String,Object> app:cachedCatalogue)if(remote.equals(app.get("origin")))count++;}
            long refreshed=index.isFile()?index.lastModified()/1000L:0;
            boolean enabled=!fields[3].toLowerCase(Locale.ROOT).contains("disabled");
            result.put(new JSONObject().put("id",remote).put("url",fields[1]).put("label",fields[2]).put("official",remote.equals("flathub")&&fields[1].equals("https://dl.flathub.org/repo/")).put("broken",brokenRemotes.getOrDefault(remote,false)).put("enabled",enabled).put("appCount",count).put("lastRefreshed",refreshed));
        }
        return result;
    }
    private List<Map<String,Object>> catalogue() throws Exception {
        if(cachedCatalogue!=null)return cachedCatalogue;
        List<Map<String,Object>> result=new ArrayList<>();JSONArray remotes=repositories();Exception last=null;int readable=0;
        for(int i=0;i<remotes.length();i++) {
            JSONObject repository=remotes.getJSONObject(i);if(!repository.optBoolean("enabled",true))continue;
            String remote=repository.getString("id");validateRemote(remote);
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
                row.put("badges",badges(row));
                row.put("packageName",org.matonos.linux.stub.DeployManifest.packageForPublic((String)row.get("ref")));result.add(row);
            }}
        }
        if(readable==0&&last!=null)throw last;
        cachedCatalogue=result;return result;
    }
    private void addVerifiedBadge(Map<String,Object> row) {
        if(!"flathub".equals(row.get("origin")))return;
        String ref=String.valueOf(row.getOrDefault("ref",row.get("id")));String[] parts=ref.split("/");if(parts.length<2)return;String appId=parts[1];Boolean verified=verifiedApps.get(appId);
        if(verified==null)try {JSONObject result=getJson("https://flathub.org/api/v2/verification/"+URLEncoder.encode(appId,StandardCharsets.UTF_8)+"/status");verified=result.optBoolean("verified",false)||"verified".equalsIgnoreCase(result.optString("status"));verifiedApps.put(appId,verified);}
        catch(Exception error){Log.e(TAG,"Flathub verification lookup app="+appId+" failed",error);verifiedApps.put(appId,false);verified=false;}
        if(verified){@SuppressWarnings("unchecked") List<Map<String,Object>> badges=(List<Map<String,Object>>)row.computeIfAbsent("badges",key->new ArrayList<>());badges.add(Map.of("id","verified","label","Verified developer","icon","verified"));}
    }
    private List<Map<String,Object>> badges(Map<String,Object> row) {
        List<Map<String,Object>> badges=new ArrayList<>();String remote=String.valueOf(row.get("origin"));
        if(remote.equals("flatpark"))badges.add(Map.of("id","third-party","label","Third-party","icon","warning"));
        String license=String.valueOf(row.getOrDefault("license",""));
        if(license.matches("(?i).*(GPL|LGPL|AGPL|MIT|Apache|BSD|MPL|CC0|ISC|Zlib).*"))badges.add(Map.of("id","open-source","label","Open source","icon","code"));
        Object added=row.get("added");if(added instanceof Number&&((Number)added).longValue()>System.currentTimeMillis()/1000L-30L*86400)badges.add(Map.of("id","new","label","New","icon","fiber_new"));
        return badges;
    }
    private List<Map<String,Object>> collectionListings(String kind) throws Exception {
        if(!"popular".equals(kind)&&!"new".equals(kind))return Collections.emptyList();
        synchronized(cachedCollections){List<Map<String,Object>> cached=cachedCollections.get(kind);if(cached!=null)return cached;}
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
            addVerifiedBadge(row);Map<String,Object> listing=AppStreamCatalogue.search(Collections.singletonList(row),"").get(0);
            listing.put("added",SourceTypes.epochSeconds(hit.opt("added"),hit.opt("timestamp"),hit.opt("date")));
            JSONObject rating=ratingFor(hit.optString("app_id"));if(rating!=null)listing.put("rating",rating);
            result.add(listing);
        }
        synchronized(cachedCollections){cachedCollections.put(kind,result);}return result;
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
        double denominator=Math.log1p(maximum);List<PopularApp> result=new ArrayList<>();
        for(int i=0;i<accepted.size()&&result.size()<limit;i++) {
            JSONObject hit=accepted.get(i);Map<String,Object> row=appsById.get(hit.optString("app_id"));
            addVerifiedBadge(row);Map<String,Object> listing=AppStreamCatalogue.search(Collections.singletonList(row),"").get(0);
            JSONObject rating=ratingFor(hit.optString("app_id"));if(rating!=null)listing.put("rating",rating);
            long installs=hit.optLong("_matonos_installs");
            double score=denominator>0?10.0*Math.log1p(installs)/denominator:10.0*(accepted.size()-i)/(double)Math.max(1,accepted.size());
            listing.put("badges",row.get("badges"));PopularApp item=new PopularApp();item.app=SourceTypes.listings(Collections.singletonList(listing),0,1)[0];
            item.score=Math.round(score*10.0)/10.0;result.add(item);
        }
        return result.toArray(new PopularApp[0]);
    }
    private List<Map<String,Object>> withRatings(List<Map<String,Object>> rows) {
        for(Map<String,Object> row:rows) {
            Object rawId=row.get("id");if(!(rawId instanceof String))continue;
            String id=(String)rawId;if(id.startsWith("app/")){String[] parts=id.split("/");if(parts.length>1)id=parts[1];}
            JSONObject rating=ratingFor(id);if(rating!=null)row.put("rating",rating);addVerifiedBadge(row);
        }
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
            Map<String,Object> visible=AppStreamCatalogue.search(Collections.singletonList(listing),"").get(0);
            JSONObject rating=ratingFor(parts[1]);if(rating!=null)visible.put("rating",rating);
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
