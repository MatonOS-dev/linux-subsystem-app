import org.matonos.linuxhost.stubgen.PackSessionPlan;
import java.util.*;

public class PackSessionPlanTest {
    static String a="pack_"+"a".repeat(64), b="pack_"+"b".repeat(64), c="pack_"+"c".repeat(64);
    static List<String> list(String... s){return Arrays.asList(s);}
    static void check(boolean value){if(!value)throw new AssertionError();}
    interface Action {void run() throws Exception;}
    static void rejects(Action action)throws Exception {
        try{action.run();}catch(java.io.IOException expected){return;}
        throw new AssertionError("Expected rejection");
    }
    static class Fake implements PackSessionPlan.Installer {
        List<String> calls=new ArrayList<>();String fail;
        void call(String value)throws Exception{calls.add(value);if(value.equals(fail))throw new java.io.IOException(value);}
        public void begin(PackSessionPlan.Mode mode,boolean keep)throws Exception{call(mode+":"+keep);}
        public void write(String apk)throws Exception{call("write:"+apk);}
        public void remove(String split)throws Exception{call("remove:"+split);}
        public void commit()throws Exception{call("commit");}
        public void abandon()throws Exception{call("abandon");}
    }
    public static void main(String[] args)throws Exception {
        PackSessionPlan first=PackSessionPlan.create(false,0,1,list(),list(b,a),list(a,b),true);
        check(first.mode==PackSessionPlan.Mode.FULL && !first.dontKillApp);
        check(first.write.equals(list("base.apk",a+".apk",b+".apk")) && first.remove.isEmpty());
        PackSessionPlan update=PackSessionPlan.create(true,1,1,list(a,b),list(a,c),list(c),true);
        check(update.mode==PackSessionPlan.Mode.INHERIT && !update.dontKillApp);
        check(update.write.equals(list("base.apk",c+".apk")) && update.remove.equals(list(b)));
        Fake fake=new Fake();update.submit(fake);
        check(fake.calls.equals(list("INHERIT:false","write:base.apk","write:"+c+".apk","remove:"+b,"commit")));
        for(String fail:list("write:base.apk","remove:"+b,"commit")) {
            Fake broken=new Fake();broken.fail=fail;rejects(()->update.submit(broken));
            check(broken.calls.get(broken.calls.size()-1).equals("abandon"));
        }
        Fake cancelled=new Fake();
        rejects(()->update.submit(cancelled,()->true));check(cancelled.calls.isEmpty());
        Fake during=new Fake();
        rejects(()->update.submit(during,()->during.calls.size()>=2));
        check(during.calls.equals(list("INHERIT:false","write:base.apk","abandon")));
        check(PackSessionPlan.create(true,1,1,list(a),list(a),list(),false).dontKillApp);
        rejects(()->PackSessionPlan.create(true,1,2,list(a),list(a),list(a),true));
        rejects(()->PackSessionPlan.create(true,2,1,list(a),list(a),list(a),true));
        rejects(()->PackSessionPlan.create(false,0,0,list(),list(),list(),true));
        rejects(()->PackSessionPlan.create(false,0,1,list(),list(a),list(),true));
        rejects(()->PackSessionPlan.create(true,1,1,list(a,a),list(a),list(),true));
        rejects(()->PackSessionPlan.create(true,1,1,list(a),list("../bad"),list(),true));
        rejects(()->PackSessionPlan.create(false,0,1,list(),list(),list(),false));
        System.out.println("Pack session planning tests passed");
    }
}
