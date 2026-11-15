package org.matonos.linux;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import java.util.zip.GZIPOutputStream;
import org.matonos.linux.store.*;

public final class StoreSourceHostTest {
    private static void check(boolean condition,String message){if(!condition)throw new AssertionError(message);}
    private static void catalogue() throws Exception {
        String xml="<components><component type='desktop-application'><id>com.example.App.desktop</id>"
                +"<name xml:lang='fr'>Autre</name><name>Example</name><summary>Editor</summary><description><p>Rich <em>text</em></p></description>"
                +"<developer><name>Example developers</name></developer><project_license>MIT</project_license>"
                +"<bundle type='flatpak'>app/com.example.App/x86_64/stable</bundle>"
                +"<icon type='remote'>https://example.org/icon.png</icon><screenshots><screenshot><image type='thumbnail'>thumb</image><image type='source'>screen</image></screenshot></screenshots>"
                +"<content_rating type='oars-1.1'><content_attribute id='violence-cartoon'>mild</content_attribute></content_rating>"
                +"<releases><release version='2.0'/></releases></component>"
                +"<component><id>runtime</id><bundle type='flatpak'>runtime/org.example.Platform/x86_64/1</bundle></component></components>";
        ByteArrayOutputStream output=new ByteArrayOutputStream();try(GZIPOutputStream gz=new GZIPOutputStream(output)){gz.write(xml.getBytes(StandardCharsets.UTF_8));}
        List<Map<String,Object>> rows=AppStreamCatalogue.read(new ByteArrayInputStream(output.toByteArray()),true,"test");
        check(rows.size()==1,"only app components");Map<String,Object> row=rows.get(0);
        check(row.get("id").equals("com.example.App"),"bundle ID, not desktop suffix");check(row.get("name").equals("Example"),"unlocalized fallback");
        check(row.get("description").equals("Rich text"),"description markup text");check(row.get("developer").equals("Example developers"),"developer");
        check(row.get("screenshots").equals(List.of("screen")),"full screenshots");check(row.containsKey("ageRating"),"OARS attributes");
        check(AppStreamCatalogue.search(rows,"EDITOR").size()==1,"case insensitive search");
        check(!AppStreamCatalogue.search(rows,"").get(0).containsKey("description"),"search returns basic listing");
        try {AppStreamCatalogue.read(new ByteArrayInputStream("<!DOCTYPE components [<!ENTITY e SYSTEM 'file:///etc/passwd'>]><components/>".getBytes()),false,"bad");throw new AssertionError("DOCTYPE accepted");}
        catch(org.xml.sax.SAXException expected) { }
    }
    private static List<CommitChain.Package> steam() {
        String runtime="org.freedesktop.Platform",locale="org.freedesktop.Platform.Locale",gl="org.freedesktop.Platform.GL.default",compat="org.freedesktop.Platform.Compat.i386",steam="com.valvesoftware.Steam";
        // Deliberately put the dependant first to exercise ordering.
        return List.of(item(steam,List.of(runtime,locale,gl,compat),false),item(runtime,List.of(locale,gl),true),
                item(locale,List.of(),false),item(gl,List.of(),false),item(compat,List.of(),false));
    }
    private static CommitChain.Package item(String name,List<String> dependencies,boolean inherit) {
        return new CommitChain.Package(name,dependencies,new String[]{"base.apk","index_abc.apk","erofs_def.apk"},inherit?new String[]{"erofs_old"}:new String[0]);
    }
    private static void chain(boolean fail) throws Exception {
        List<String> committed=new ArrayList<>();boolean[] terminal={false,false};
        CommitChain.run(steam(),new CommitChain.Sink(){
            public void commit(CommitChain.Package item) throws Exception {
                check(committed.containsAll(item.dependencies),"dependency not committed before "+item.name);
                check(item.names.length==3,"base and splits included");
                if(item.remove.length>0)check(item.remove.length==1,"obsolete splits removed on update");
                CompletableFuture<Boolean> result=new CompletableFuture<>();
                Thread callback=new Thread(()->result.complete(!(fail&&item.name.equals("org.freedesktop.Platform"))));callback.start();
                if(!result.get())throw new IOException("Runtime rejected by PackageInstaller");callback.join();committed.add(item.name);
            }
            public void done(){terminal[0]=true;}
            public void failed(Exception error){terminal[1]=true;}
        });
        check(terminal[0]!=terminal[1],"exactly one terminal event");
        if(fail){check(terminal[1],"failed sent");check(!committed.contains("com.valvesoftware.Steam"),"Steam must never commit after runtime failure");}
        else{check(committed.size()>=5,"Steam and at least four runtime/extension commits");check(terminal[0],"done sent");}
    }
    private static void invalidGraph() throws Exception {
        int[] commits={0},failures={0};
        CommitChain.run(List.of(item("a",List.of("missing"),false)),new CommitChain.Sink(){
            public void commit(CommitChain.Package p){commits[0]++;}public void done(){throw new AssertionError();}public void failed(Exception e){failures[0]++;}
        });check(commits[0]==0&&failures[0]==1,"validate before first commit");
    }
    private static void extraData(boolean invalid) throws Exception {
        List<String> events=new ArrayList<>();
        CommitChain.run(List.of(item("extra-app",List.of(),false)),new CommitChain.Sink(){
            public void commit(CommitChain.Package p) {
                events.add(Arrays.asList(p.names).contains("base.apk")?"stub":"split");
                if(events.size()==2)check(p.names.length==1&&p.remove.length==1,"one replacement image split, preserving base and normal splits");
            }
            public CommitChain.Followup followup(CommitChain.Package p) {return ()->{
                events.add("stub apply_extra");events.add("OSTree validation");
                if(invalid)throw new IOException("returned image differs from OSTree");
                return new CommitChain.Package(p.name,List.of(),new String[]{"erofs_extra.apk"},new String[]{"erofs_normal"});
            };}
            public void done(){events.add("done");}public void failed(Exception e){events.add("failed");}
        });
        check(events.equals(invalid?List.of("stub","stub apply_extra","OSTree validation","failed"):
                List.of("stub","stub apply_extra","OSTree validation","split","done")),"extra-data ordering/validation failure: "+events);
    }
    private static void sourceContract() throws Exception {
        List<Integer> rows=List.of(0,1,2,3,4);
        check(SourcePaging.page(rows,0,2).equals(List.of(0,1)),"first page");
        check(SourcePaging.page(rows,2,2).equals(List.of(2,3)),"middle page");
        check(SourcePaging.page(rows,4,2).equals(List.of(4)),"short last page");
        check(SourcePaging.page(rows,5,2).isEmpty(),"end page");
        check(SourcePaging.page(rows,Integer.MAX_VALUE,Integer.MAX_VALUE).isEmpty(),"paging overflow");
        for(int[] bad:new int[][]{{-1,2},{0,0},{0,-1}})try{SourcePaging.page(rows,bad[0],bad[1]);throw new AssertionError("invalid paging accepted");}catch(IllegalArgumentException expected){}
        List<Map<String,Object>> candidates=List.of(Map.of("id","app/example/x86_64/stable","origin","alpha"),Map.of("id","app/example/x86_64/stable","origin","beta"));
        check(SourceCandidates.find(candidates,"app/example/x86_64/stable","beta").get("origin").equals("beta"),"selected remote routes install");
        try{SourceCandidates.find(candidates,"app/example/x86_64/stable","missing");throw new AssertionError("remote fallback");}catch(IOException expected){}
        check(SourceCandidates.install("app/example/x86_64/stable","beta").get("origin").equals("beta"),"install routes supplied remote without catalogue fallback");
        try{SourceCandidates.install("app/example/x86_64/stable","--bad");throw new AssertionError("invalid remote accepted");}catch(IllegalArgumentException expected){}
        SourceOperations table=new SourceOperations();check(!table.running("unknown"),"unknown idle");table.cancel("unknown");
        SourceOperations.Operation operation=table.start("test");
        try{table.start("test");throw new AssertionError("duplicate accepted");}catch(IllegalArgumentException expected){}
        CountDownLatch stuck=new CountDownLatch(1),entered=new CountDownLatch(1);
        Thread worker=new Thread(()->{entered.countDown();try{stuck.await();}catch(InterruptedException cancelled){}});operation.worker=worker;worker.start();entered.await();
        check(table.running("test"),"stuck worker still running");table.cancel("test");worker.join(2000);check(!worker.isAlive()&&operation.cancelled(),"cancel interrupts work");
        check(table.running("test"),"cancelled operation retained until terminal callback");
        int[] cleanup={0};operation.onCancel(()->cleanup[0]++);check(cleanup[0]==1,"late cancel resource cleaned");
        check(operation.finish()&&!operation.finish()&&!table.running("test"),"terminal event once");table.cancel("test");
        SourceOperations.Operation success=table.start("success");check(success.claimTerminal()&&!success.claimTerminal(),"terminal delivery claimed once");check(table.running("success"),"running until terminal callback dispatched");table.cancel("success");check(!success.cancelled(),"completion wins late cancellation");success.finish();check(!table.running("success"),"done idle");
        SourceOperations.Operation failure=table.start("failure");failure.finish();check(!table.running("failure"),"failed idle");
        SourceOperations.Operation download=table.start("download");Process process=new ProcessBuilder("sleep","60").start();download.process(process);Process parallel=new ProcessBuilder("sleep","60").start();download.process(parallel);table.cancel("download");check(process.waitFor(2,TimeUnit.SECONDS)&&parallel.waitFor(2,TimeUnit.SECONDS),"cancel terminates every download process");download.release(process);download.release(parallel);download.finish();
        StoreRepair repair=new StoreRepair("app/example/x86_64/stable","runtime/missing/x86_64/1");
        repair.request((id,result)->{check(id.equals("app/example/x86_64/stable"),"repair requests installed app ID");result.result(true,null);});repair.await(1,TimeUnit.SECONDS);
        StoreRepair rejected=new StoreRepair("app/id","runtime/id");rejected.request((id,result)->result.result(false,"source disabled"));
        try{rejected.await(1,TimeUnit.SECONDS);throw new AssertionError("store refusal ignored");}catch(IOException expected){check(expected.getMessage().contains("source disabled")&&expected.getMessage().contains("app/id")&&expected.getMessage().contains("runtime/id"),"repair refusal context");}
        StoreRepair crashed=new StoreRepair("app/id","runtime/id");crashed.request((id,result)->{throw new IOException("binder failed");});
        try{crashed.await(1,TimeUnit.SECONDS);throw new AssertionError("request failure ignored");}catch(IOException expected){check(expected.getMessage().contains("binder failed"),"request exception propagated");}
    }
    public static void main(String[] args) throws Exception {
        sourceContract();catalogue();chain(false);chain(true);invalidGraph();extraData(false);extraData(true);System.out.println("StoreSourceHostTest: PASS (paging, remote routing, cancel/process termination, running states, store repair, AppStream, five-package Steam success/failure, missing dependency, extra-data success/validation failure)");}
}
