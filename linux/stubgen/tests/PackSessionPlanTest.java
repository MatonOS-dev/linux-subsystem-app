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
        public void begin(PackSessionPlan.Mode mode)throws Exception{call(mode.toString());}
        public void write(String apk)throws Exception{call("write:"+apk);}
        public void remove(String split)throws Exception{call("remove:"+split);}
        public void commit()throws Exception{call("commit");}
        public void abandon()throws Exception{call("abandon");}
    }
    static void base(java.io.File file,String contents,boolean signature) throws Exception {
        try(java.util.zip.ZipOutputStream zip=new java.util.zip.ZipOutputStream(new java.io.FileOutputStream(file))) {
            zip.putNextEntry(new java.util.zip.ZipEntry("AndroidManifest.xml"));zip.write(contents.getBytes(java.nio.charset.StandardCharsets.UTF_8));zip.closeEntry();
            if(signature){zip.putNextEntry(new java.util.zip.ZipEntry("META-INF/CERT.SF"));zip.write(1);zip.closeEntry();}
        }
    }
    public static void main(String[] args)throws Exception {
        PackSessionPlan first=PackSessionPlan.create(false,0,1,list(),list(b,a),list(a,b),true);
        check(first.mode==PackSessionPlan.Mode.FULL);
        check(first.write.equals(list("base.apk",a+".apk",b+".apk")) && first.remove.isEmpty());
        PackSessionPlan update=PackSessionPlan.create(true,1,1,list(a,b),list(a,c),list(c),false);
        check(update.mode==PackSessionPlan.Mode.INHERIT);
        check(update.write.equals(list(c+".apk")) && update.remove.equals(list(b)));
        Fake fake=new Fake();update.submit(fake);
        check(fake.calls.equals(list("INHERIT","write:"+c+".apk","remove:"+b,"commit")));
        for(String fail:list("write:"+c+".apk","remove:"+b,"commit")) {
            Fake broken=new Fake();broken.fail=fail;rejects(()->update.submit(broken));
            check(broken.calls.get(broken.calls.size()-1).equals("abandon"));
        }
        Fake cancelled=new Fake();
        rejects(()->update.submit(cancelled,()->true));check(cancelled.calls.isEmpty());
        Fake during=new Fake();
        rejects(()->update.submit(during,()->during.calls.size()>=2));
        check(during.calls.equals(list("INHERIT","write:"+c+".apk","abandon")));
        check(PackSessionPlan.create(true,1,1,list(a),list(a),list(),false).mode==PackSessionPlan.Mode.INHERIT);
        check(PackSessionPlan.create(true,1,2,list(a),list(a),list(a),true).mode==PackSessionPlan.Mode.FULL);
        rejects(()->PackSessionPlan.create(true,1,2,list(a),list(a),list(a),false));
        rejects(()->PackSessionPlan.create(true,2,1,list(a),list(a),list(a),true));
        rejects(()->PackSessionPlan.create(false,0,0,list(),list(),list(),true));
        rejects(()->PackSessionPlan.create(false,0,1,list(),list(a),list(),true));
        rejects(()->PackSessionPlan.create(true,1,1,list(a,a),list(a),list(),true));
        rejects(()->PackSessionPlan.create(true,1,1,list(a),list("../bad"),list(),true));
        rejects(()->PackSessionPlan.create(false,0,1,list(),list(),list(),false));
        String ix="index_"+"d".repeat(64), next="index_"+"e".repeat(64);
        PackSessionPlan index=PackSessionPlan.create(true,17,17,list(a,ix),list(a,next),list(next),false);
        check(index.write.equals(list(next+".apk")) && index.remove.equals(list(ix)));
        PackSessionPlan full=PackSessionPlan.create(true,17,18,list(a,ix),list(a,next),list(a,next),true);
        check(full.mode==PackSessionPlan.Mode.FULL && full.remove.isEmpty());
        java.io.File imageA=java.io.File.createTempFile("erofs-a-",".img"),imageB=java.io.File.createTempFile("erofs-b-",".img");
        try {
            byte[] imageContents="identical deterministic EROFS image".getBytes(java.nio.charset.StandardCharsets.UTF_8);
            java.nio.file.Files.write(imageA.toPath(),imageContents);java.nio.file.Files.write(imageB.toPath(),imageContents);
            String erofs=org.matonos.linuxhost.stubgen.StubGenerator.erofsSplitName(imageA);
            check(erofs.equals(org.matonos.linuxhost.stubgen.StubGenerator.erofsSplitName(imageB)));
            // Split names are scoped to one PackageInstaller package. Identical
            // images in the app and a dependency therefore remain valid.
            PackSessionPlan app=PackSessionPlan.create(false,0,1,list(),list(erofs),list(erofs),true);
            PackSessionPlan dependency=PackSessionPlan.create(false,0,1,list(),list(erofs),list(erofs),true);
            check(app.write.equals(list("base.apk",erofs+".apk")));
            check(dependency.write.equals(list("base.apk",erofs+".apk")));
            // Reusing an installed EROFS split must keep it without rewriting it.
            PackSessionPlan retained=PackSessionPlan.create(true,1,1,list(erofs),list(erofs),list(),false);
            check(retained.mode==PackSessionPlan.Mode.INHERIT && retained.write.isEmpty() && retained.remove.isEmpty());
            rejects(()->PackSessionPlan.create(false,0,1,list(),list(erofs,erofs),list(erofs),true));
        } finally {imageA.delete();imageB.delete();}
        check(PackSessionPlan.nextRevision(17,17,0,new int[]{2,5,3})==6);
        check(PackSessionPlan.nextRevision(17,18,0,new int[]{Integer.MAX_VALUE})==1);
        rejects(()->PackSessionPlan.nextRevision(17,17,0,new int[]{Integer.MAX_VALUE}));
        rejects(()->PackSessionPlan.nextRevision(17,17,0,new int[]{-1}));
        java.io.File x=java.io.File.createTempFile("base-a-",".apk"),y=java.io.File.createTempFile("base-b-",".apk");
        try {
            base(x,"template",false);base(y,"template",true);check(PackSessionPlan.sameBase(x,y));
            base(y,"changed permissions",false);check(!PackSessionPlan.sameBase(x,y));
        } finally {x.delete();y.delete();}
        System.out.println("Pack session planning tests passed");
    }
}
