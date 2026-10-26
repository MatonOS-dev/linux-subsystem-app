import org.matonos.linuxhost.stubgen.*;
import java.io.*;
import java.util.*;

public final class DependencyPlanTest {
    static final String APP="app/org.test.App/x86_64/stable",RUNTIME="runtime/org.test.Platform/x86_64/stable",GL="runtime/org.test.GL/x86_64/stable";
    static final String FIRST,OTHER;static final byte[] OBJECT={1,2,3};static final String COMMIT;
    static {try{FIRST=new RemoteOrigin("first","https://first.example/repo","AQID").encode();OTHER=new RemoteOrigin("other","https://other.example/repo","AQID").encode();COMMIT=ObjectPacks.hex(java.security.MessageDigest.getInstance("SHA-256").digest(OBJECT));}catch(Exception e){throw new ExceptionInInitializerError(e);}}
    static DependencyPlan.Info info(String runtime,String... related)throws Exception{return new DependencyPlan.Info(COMMIT,"[Runtime]\n".getBytes(),runtime,Arrays.asList(related));}
    static DependencyPlan.Info infoMetadata(String metadata,String runtime,String... related)throws Exception{return new DependencyPlan.Info(COMMIT,metadata.getBytes(java.nio.charset.StandardCharsets.UTF_8),runtime,Arrays.asList(related));}
    static DependencyPlan.Info extension(String parent)throws Exception{return new DependencyPlan.Info(COMMIT,"[ExtensionOf]\n".getBytes(),"",parent,List.of(),false);}
    static DeploymentRecord record(String ref,String origin,DeploymentRecord.Dependency... dependencies)throws Exception{return new DeploymentRecord(ref,COMMIT,origin,"[Runtime]\n".getBytes(),new byte[]{1},OBJECT,Arrays.asList(dependencies));}
    static class Fake implements DependencyPlan.Backend {
        final Map<String,DeploymentRecord> installed=new HashMap<>();final Map<String,String> recorded=new HashMap<>();
        final Map<String,DependencyPlan.Info> refs=new HashMap<>();final List<String> queries=new ArrayList<>();
        final List<List<String>> choices=new ArrayList<>();boolean cancel,fail;int selection;
        public DeploymentRecord installed(String ref){return installed.get(ref);}
        public String recordedOrigin(String ref){return recorded.get(ref);}
        public DependencyPlan.Info inspect(String ref,String origin)throws Exception {queries.add(ref+"@"+origin);if(fail&&!ref.equals(APP))throw new IOException("Network/authentication failure");return refs.get(ref+"@"+origin);}
        public String choose(String ref,List<String> origins){choices.add(origins);return cancel?null:origins.get(selection);}
        Fake add(String ref,String origin,DependencyPlan.Info info){refs.put(ref+"@"+origin,info);return this;}
        List<DependencyPlan.Node> plan(boolean repair)throws Exception{return new DependencyPlan(FIRST,List.of(OTHER),this).build(APP,FIRST,repair);}
    }
    interface Checked{void run()throws Exception;}
    static void rejects(Checked code)throws Exception{try{code.run();throw new AssertionError("Expected failure");}catch(IOException expected){}}
    static DependencyPlan.Node node(List<DependencyPlan.Node> nodes,String ref){return nodes.stream().filter(n->n.ref.equals(ref)).findFirst().orElseThrow();}
    static void check(boolean condition){check(condition,"check failed");}
    static void check(boolean condition,String message){if(!condition)throw new AssertionError(message);}
    public static void main(String[] args)throws Exception {
        // Related refs with subpaths (Locale with a language subset): plan by ref, install only those subpaths.
        check(DependencyPlan.refsOnly(List.of("runtime/a/x86_64/1\n/en","runtime/b/x86_64/1")).equals(List.of("runtime/a/x86_64/1","runtime/b/x86_64/1")));
        String locale="runtime/org.test.Platform.Locale/x86_64/stable";
        Fake languages=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable"))
                .add(RUNTIME,FIRST,info("",locale+"\n/en\n/de")).add(locale,FIRST,info(""));
        List<DependencyPlan.Node> languagePlan=languages.plan(false);
        check(node(languagePlan,locale).subpaths.equals(List.of("/en","/de")));
        check(node(languagePlan,RUNTIME).subpaths.isEmpty());
        Fake reuse=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable"));
        reuse.installed.put(RUNTIME,record(RUNTIME,OTHER));
        List<DependencyPlan.Node> plan=reuse.plan(false);
        check(node(plan,RUNTIME).reused&&node(plan,RUNTIME).origin.equals(OTHER));
        check(reuse.queries.equals(List.of(APP+"@"+FIRST))&&reuse.choices.isEmpty());
        check(plan.stream().filter(n->!n.reused).count()==1); // content pulls exclude installed refs

        Fake multiple=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable"))
                .add(RUNTIME,FIRST,info("",GL)).add(RUNTIME,"flathub",info("",GL)).add(RUNTIME,OTHER,info("",GL))
                .add(GL,OTHER,info(""));multiple.selection=1;
        plan=multiple.plan(false);
        check(multiple.choices.equals(List.of(List.of(FIRST,"flathub",OTHER))));
        check(node(plan,RUNTIME).origin.equals("flathub")&&node(plan,GL).origin.equals(OTHER));
        check(plan.indexOf(node(plan,RUNTIME))<plan.indexOf(node(plan,GL))&&plan.indexOf(node(plan,GL))<plan.indexOf(node(plan,APP)));

        Fake repair=new Fake();
        repair.installed.put(APP,record(APP,FIRST,new DeploymentRecord.Dependency(RUNTIME,OTHER,"/usr")));
        repair.recorded.put(RUNTIME,OTHER);repair.add(RUNTIME,OTHER,info(""));
        plan=repair.plan(true);
        check(!node(plan,APP).reused&&!node(plan,RUNTIME).reused&&node(plan,RUNTIME).origin.equals(OTHER));
        check(repair.queries.equals(List.of(RUNTIME+"@"+OTHER))&&repair.choices.isEmpty());

        Fake removedRuntime=new Fake().add(RUNTIME,OTHER,info(""));
        removedRuntime.installed.put(APP,record(APP,FIRST,new DeploymentRecord.Dependency(RUNTIME,OTHER,"/usr")));
        // The parent's signed record is the durable origin source even when the
        // runtime stub itself has been uninstalled and no installed record can
        // report its origin.
        plan=removedRuntime.plan(true);
        check(!node(plan,RUNTIME).reused&&node(plan,RUNTIME).origin.equals(OTHER));
        check(removedRuntime.queries.equals(List.of(RUNTIME+"@"+OTHER)));

        Fake nested=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable"));
        nested.installed.put(RUNTIME,record(RUNTIME,OTHER,new DeploymentRecord.Dependency(GL,"flathub","/usr/lib/GL")));
        nested.recorded.put(GL,"flathub");nested.add(GL,"flathub",info(""));
        plan=nested.plan(false);check(node(plan,RUNTIME).reused&&!node(plan,GL).reused);
        check(nested.queries.equals(List.of(APP+"@"+FIRST,GL+"@flathub")));

        Fake undeclaredRepair=new Fake(){
            @Override public DependencyPlan.Info recordedInfo(DeploymentRecord record)throws Exception{return info("org.test.Platform/x86_64/stable");}
        };
        undeclaredRepair.installed.put(APP,record(APP,FIRST));undeclaredRepair.add(RUNTIME,OTHER,info(""));
        check(node(undeclaredRepair.plan(true),RUNTIME).origin.equals(OTHER));
        Fake crossRemoteExtension=new Fake(){
            @Override public List<String> related(String ref,String origin,DependencyPlan.Info info){return ref.equals(APP)?List.of(GL):info.related;}
        };
        crossRemoteExtension.add(APP,FIRST,info("")).add(GL,OTHER,info(""));
        check(node(crossRemoteExtension.plan(false),GL).origin.equals(OTHER));
        String gnomeRuntime="runtime/org.gnome.Platform/x86_64/46",extension="runtime/org.freedesktop.Platform.GL.default/x86_64/26.08",parent="runtime/org.freedesktop.Platform/x86_64/26.08";
        Fake extensionParent=new Fake(){
            @Override public List<String> related(String ref,String origin,DependencyPlan.Info info){return ref.equals(APP)?List.of(extension):info.related;}
        };
        extensionParent.add(APP,FIRST,info("org.gnome.Platform/x86_64/46"))
                .add(gnomeRuntime,FIRST,info(""))
                .add(extension,OTHER,extension(parent))
                .add(parent,FIRST,info("", "runtime/org.freedesktop.Platform.Locale/x86_64/26.08"))
                .add("runtime/org.freedesktop.Platform.Locale/x86_64/26.08",FIRST,info(""));
        plan=extensionParent.plan(false);
        check(node(plan,extension).stageDependencies.contains(parent),"extension parent was not staged: "+node(plan,extension).stageDependencies+" / "+node(plan,extension).dependencies);
        check(!node(plan,extension).dependencies.contains(parent));
        check(plan.stream().noneMatch(n->n.ref.equals("runtime/org.freedesktop.Platform.Locale/x86_64/26.08")));
        check(plan.indexOf(node(plan,parent))<plan.indexOf(node(plan,extension)));
        check(node(plan,parent).origin.equals(FIRST));
        // GNOME Platform 51 may use extensions built against freedesktop 26.08.
        // That runtime is staged for apply_extra, but its own Locale is not part
        // of the GNOME app's launch package graph.
        String gnome51="runtime/org.gnome.Platform/x86_64/51";
        String gnomeLocale="runtime/org.gnome.Platform.Locale/x86_64/51";
        String fdGl="runtime/org.freedesktop.Platform.GL.default/x86_64/26.08";
        String fdMaton="runtime/org.freedesktop.Platform.GL.maton/x86_64/26.08-extra";
        String fdCodecs="runtime/org.freedesktop.Platform.codecs-extra/x86_64/26.08";
        String fdLocale="runtime/org.freedesktop.Platform.Locale/x86_64/26.08";
        Fake gnome=new Fake(){
            @Override public List<String> related(String ref,String origin,DependencyPlan.Info info) {
                if(ref.equals(gnome51))return List.of(gnomeLocale+"\n/en",fdGl,fdMaton,fdCodecs);
                if(ref.equals(parent))return List.of(fdLocale);
                return info.related;
            }
        };
        gnome.add(APP,FIRST,info("org.gnome.Platform/x86_64/51"))
                .add(gnome51,FIRST,info(""))
                .add(gnomeLocale,FIRST,extension(gnome51))
                .add(fdGl,OTHER,extension(parent)).add(fdMaton,OTHER,extension(parent))
                .add(fdCodecs,OTHER,extension(parent))
                .add(parent,FIRST,info(""))
                .add(fdLocale,OTHER,extension(parent));
        plan=gnome.plan(false);
        check(node(plan,gnomeLocale).subpaths.equals(List.of("/en")));
        check(plan.stream().noneMatch(n->n.ref.equals(fdLocale)));
        check(!node(plan,fdGl).dependencies.contains(parent)&&node(plan,fdGl).stageDependencies.contains(parent));
        check(node(plan,gnome51).dependencies.contains(gnomeLocale));
        // Platform 26.08's actual extension metadata has GL versions in
        // priority order, while codecs-extra has one fixed extra branch.
        String platformMetadata=java.nio.file.Files.readString(java.nio.file.Path.of("linux/stubgen/tests/fixtures/platform-26.08.metadata"));
        String platform="runtime/org.freedesktop.Platform/x86_64/26.08";
        String glDefault="runtime/org.freedesktop.Platform.GL.default/x86_64/26.08";
        String glDefaultExtra="runtime/org.freedesktop.Platform.GL.default/x86_64/26.08-extra";
        String glMaton="runtime/org.freedesktop.Platform.GL.maton/x86_64/26.08-extra";
        String gl32="runtime/org.freedesktop.Platform.GL32.default/x86_64/26.08";
        String gl32Extra="runtime/org.freedesktop.Platform.GL32.default/x86_64/26.08-extra";
        String compat="runtime/org.freedesktop.Platform.Compat.i386/x86_64/26.08";
        String codecs="runtime/org.freedesktop.Platform.codecs-extra/x86_64/26.08-extra";
        String codecsWrong="runtime/org.freedesktop.Platform.codecs-extra/x86_64/26.08";
        Fake versioned=new Fake().add(APP,FIRST,infoMetadata("[Application]\nruntime=org.freedesktop.Platform/x86_64/26.08\n","org.freedesktop.Platform/x86_64/26.08"))
                .add(platform,FIRST,infoMetadata(platformMetadata,"",glDefault,glDefaultExtra,glMaton,gl32,gl32Extra,compat,codecs,codecsWrong))
                .add(glDefault,OTHER,extension(platform)).add(glDefaultExtra,OTHER,extension(platform))
                .add(glMaton,OTHER,extension(platform)).add(gl32,OTHER,extension(platform))
                .add(gl32Extra,OTHER,extension(platform)).add(compat,OTHER,extension(platform))
                .add(codecs,OTHER,extension(platform)).add(codecsWrong,OTHER,extension(platform));
        plan=versioned.plan(false);
        check(plan.stream().anyMatch(n->n.ref.equals(glDefault))&&!plan.stream().anyMatch(n->n.ref.equals(glDefaultExtra)));
        check(plan.stream().anyMatch(n->n.ref.equals(gl32))&&!plan.stream().anyMatch(n->n.ref.equals(gl32Extra)));
        check(plan.stream().anyMatch(n->n.ref.equals(glMaton))&&plan.stream().anyMatch(n->n.ref.equals(compat)));
        check(plan.stream().anyMatch(n->n.ref.equals(codecs))&&!plan.stream().anyMatch(n->n.ref.equals(codecsWrong)));
        List<DeploymentRecord.Dependency> previouslyInstalledRefs=new ArrayList<>();
        for(String ref:List.of(glDefault,glDefaultExtra,glMaton,gl32,gl32Extra,compat,codecs,codecsWrong))
            previouslyInstalledRefs.add(new DeploymentRecord.Dependency(ref,FIRST,"/usr"));
        Fake reuseVersioned=new Fake().add(APP,FIRST,infoMetadata("[Application]\nruntime=org.freedesktop.Platform/x86_64/26.08\n","org.freedesktop.Platform/x86_64/26.08"));
        reuseVersioned.installed.put(platform,new DeploymentRecord(platform,COMMIT,FIRST,platformMetadata.getBytes(java.nio.charset.StandardCharsets.UTF_8),new byte[]{1},OBJECT,previouslyInstalledRefs));
        for(String ref:List.of(glDefault,glDefaultExtra,glMaton,gl32,gl32Extra,compat,codecs,codecsWrong))
            reuseVersioned.installed.put(ref,record(ref,FIRST));
        plan=reuseVersioned.plan(false);
        check(plan.stream().anyMatch(n->n.ref.equals(glDefault))&&!plan.stream().anyMatch(n->n.ref.equals(glDefaultExtra)));
        check(plan.stream().anyMatch(n->n.ref.equals(gl32))&&!plan.stream().anyMatch(n->n.ref.equals(gl32Extra)));
        check(plan.stream().anyMatch(n->n.ref.equals(codecs))&&!plan.stream().anyMatch(n->n.ref.equals(codecsWrong)));
        PlanExecutor.Outcome<Integer> staged=PlanExecutor.execute(plan,Runtime.getRuntime().availableProcessors(),ignored->1);
        check(staged.completed.containsKey(APP)&&staged.skipped.isEmpty()&&staged.failed.isEmpty());
        Fake fallback=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable")).add(RUNTIME,OTHER,info(""));
        check(node(fallback.plan(false),RUNTIME).origin.equals(OTHER)&&fallback.choices.isEmpty());
        Fake absent=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable"));rejects(()->absent.plan(false));
        Fake pinnedAbsent=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable")).add(RUNTIME,FIRST,info(""));
        pinnedAbsent.recorded.put(RUNTIME,OTHER);rejects(()->pinnedAbsent.plan(true));
        check(pinnedAbsent.queries.equals(List.of(APP+"@"+FIRST,RUNTIME+"@"+OTHER)));
        Fake freshChoice=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable")).add(RUNTIME,FIRST,info("")).add(RUNTIME,OTHER,info(""));
        freshChoice.recorded.put(RUNTIME,OTHER);
        check(node(freshChoice.plan(false),RUNTIME).origin.equals(FIRST)&&freshChoice.choices.size()==1);
        Fake cancel=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable")).add(RUNTIME,FIRST,info("")).add(RUNTIME,OTHER,info(""));
        cancel.cancel=true;rejects(()->cancel.plan(false));
        Fake network=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable"));network.fail=true;rejects(()->network.plan(false));
        Fake invalid=new Fake().add(APP,FIRST,info("../x86_64/stable"));rejects(()->invalid.plan(false));
        Fake cycle=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable")).add(RUNTIME,FIRST,info("",GL)).add(GL,FIRST,info("org.test.Platform/x86_64/stable"));
        check(cycle.plan(false).size()==3);
        Fake large=new Fake();List<String> manyRelated=new ArrayList<>();
        for(int i=0;i<200;i++) {
            String ref="runtime/org.test.Extension"+i+"/x86_64/stable";
            manyRelated.add(ref);large.add(ref,FIRST,info(""));
        }
        large.add(APP,FIRST,info("",manyRelated.toArray(new String[0])));
        check(large.plan(false).size()==201);
        byte[] largeMetadata=new byte[200*1024];Arrays.fill(largeMetadata,(byte)'x');
        check(new DependencyPlan.Info(COMMIT,largeMetadata,"",List.of()).metadata.length==largeMetadata.length);
        System.out.println("PASS dependency reuse before pulls, provider ordering/choice, nested repair origins, missing providers, cancellation, errors and cycles");
        System.out.println("PASS planning 200 related dependencies");
    }
}
