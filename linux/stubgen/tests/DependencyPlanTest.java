import org.matonos.linuxhost.stubgen.*;
import java.io.*;
import java.util.*;

public final class DependencyPlanTest {
    static final String APP="app/org.test.App/x86_64/stable",RUNTIME="runtime/org.test.Platform/x86_64/stable",GL="runtime/org.test.GL/x86_64/stable";
    static final String FIRST,OTHER;static final byte[] OBJECT={1,2,3};static final String COMMIT;
    static {try{FIRST=new RemoteOrigin("first","https://first.example/repo","AQID").encode();OTHER=new RemoteOrigin("other","https://other.example/repo","AQID").encode();COMMIT=ObjectPacks.hex(java.security.MessageDigest.getInstance("SHA-256").digest(OBJECT));}catch(Exception e){throw new ExceptionInInitializerError(e);}}
    static DependencyPlan.Info info(String runtime,String... related)throws Exception{return new DependencyPlan.Info(COMMIT,"[Runtime]\n".getBytes(),runtime,Arrays.asList(related));}
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
    static void check(boolean condition){if(!condition)throw new AssertionError();}
    public static void main(String[] args)throws Exception {
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
        check(!node(plan,APP).reused&&node(plan,RUNTIME).origin.equals(OTHER));
        check(repair.queries.equals(List.of(RUNTIME+"@"+OTHER))&&repair.choices.isEmpty());

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
        Fake fallback=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable")).add(RUNTIME,OTHER,info(""));
        check(node(fallback.plan(false),RUNTIME).origin.equals(OTHER)&&fallback.choices.isEmpty());
        Fake absent=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable"));rejects(()->absent.plan(false));
        Fake pinnedAbsent=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable")).add(RUNTIME,FIRST,info(""));
        pinnedAbsent.recorded.put(RUNTIME,OTHER);rejects(()->pinnedAbsent.plan(false));
        check(pinnedAbsent.queries.equals(List.of(APP+"@"+FIRST,RUNTIME+"@"+OTHER)));
        Fake cancel=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable")).add(RUNTIME,FIRST,info("")).add(RUNTIME,OTHER,info(""));
        cancel.cancel=true;rejects(()->cancel.plan(false));
        Fake network=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable"));network.fail=true;rejects(()->network.plan(false));
        Fake invalid=new Fake().add(APP,FIRST,info("../x86_64/stable"));rejects(()->invalid.plan(false));
        Fake cycle=new Fake().add(APP,FIRST,info("org.test.Platform/x86_64/stable")).add(RUNTIME,FIRST,info("",GL)).add(GL,FIRST,info("org.test.Platform/x86_64/stable"));
        check(cycle.plan(false).size()==3);
        System.out.println("PASS dependency reuse before pulls, provider ordering/choice, nested repair origins, missing providers, cancellation, errors and cycles");
    }
}
