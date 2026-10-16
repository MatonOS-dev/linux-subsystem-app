import org.matonos.linuxhost.stubgen.*;
import java.io.*;
import java.util.*;

public final class PackageGraphTest {
    static final String APP=DependencyPlanTest.APP,BASE=DependencyPlanTest.RUNTIME,EXT=DependencyPlanTest.GL;
    static DeploymentRecord.Dependency dep(String ref,String mount)throws Exception{return new DeploymentRecord.Dependency(ref,"flathub",mount);}
    static DeploymentRecord record(String ref,DeploymentRecord.Dependency... deps)throws Exception{return DependencyPlanTest.record(ref,"flathub",deps);}
    static void check(boolean yes){if(!yes)throw new AssertionError();}
    public static void main(String[] args)throws Exception {
        DeploymentRecord root=record(APP,dep(BASE,"/usr"));
        Map<String,DeploymentRecord> installed=new HashMap<>();
        installed.put(BASE,record(BASE,dep(EXT,"/usr/lib/GL")));installed.put(EXT,record(EXT));
        List<PackageGraph.Entry> graph=PackageGraph.resolve(root,installed::get);
        check(graph.size()==3&&graph.get(2).record.ref.equals(EXT)&&graph.get(2).mount.equals("/usr/lib/GL"));
        DependencyPlanTest.rejects(()->PackageGraph.resolve(root,ref->null));
        DependencyPlanTest.rejects(()->PackageGraph.resolve(root,ref->installed.get(EXT)));
        try{PackageGraph.resolve(root,ref->{throw new SecurityException("Untrusted signer");});throw new AssertionError();}catch(SecurityException expected){}
        // A repeated ref is one package; cycles do not cause repeated mounts.
        installed.put(EXT,record(EXT,dep(BASE,"/usr")));
        check(PackageGraph.resolve(root,installed::get).size()==3);
        installed.put(EXT,record(EXT,dep(BASE,"/usr/another")));
        DependencyPlanTest.rejects(()->PackageGraph.resolve(root,installed::get));
        System.out.println("PASS recursive shared package mounts, missing/ref-mismatched/untrusted dependencies, cycles and conflicting mounts");
    }
}
