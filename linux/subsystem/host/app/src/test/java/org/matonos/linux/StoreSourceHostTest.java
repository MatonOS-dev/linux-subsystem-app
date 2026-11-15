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
        return new CommitChain.Package(name,dependencies,inherit,new String[]{"base.apk","index_abc.apk","erofs_def.apk"},inherit?new String[]{"erofs_old"}:new String[0]);
    }
    private static void chain(boolean fail) throws Exception {
        List<String> committed=new ArrayList<>();boolean[] terminal={false,false};
        CommitChain.run(steam(),new CommitChain.Sink(){
            public void commit(CommitChain.Package item) throws Exception {
                check(committed.containsAll(item.dependencies),"dependency not committed before "+item.name);
                check(item.names.length==3,"base and splits included");
                if(item.inherit)check(item.remove.length==1,"obsolete splits removed on update");
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
    public static void main(String[] args) throws Exception {catalogue();chain(false);chain(true);invalidGraph();System.out.println("StoreSourceHostTest: PASS (AppStream, five-package Steam success/failure, missing dependency)");}
}
