import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import java.io.FileOutputStream;

/** Parse the generator's binary XML with AOSP aapt2, not a mirror parser. */
public final class ForegroundManifestTest {
    public static void main(String[] args) throws Exception {
        Class<?> desktop=Class.forName("org.matonos.linuxhost.stubgen.StubGenerator$DesktopEntry");
        Constructor<?> entry=desktop.getDeclaredConstructor(String.class,java.util.List.class);
        entry.setAccessible(true);
        Class<?> manifest=Class.forName("org.matonos.linuxhost.stubgen.StubGenerator$BinaryManifest");
        Constructor<?> ctor=manifest.getDeclaredConstructor(String.class,desktop,String.class,String.class,java.util.List.class);
        ctor.setAccessible(true);
        Object encoder=ctor.newInstance("flatpak.org.example.Test",entry.newInstance("Test",Collections.emptyList()),"app/org.example.Test/x86_64/stable","flathub",Collections.emptyList());
        Method encode=manifest.getDeclaredMethod("encode");encode.setAccessible(true);
        File apk=new File(args[1],"foreground-manifest.apk");
        try(ZipOutputStream zip=new ZipOutputStream(new FileOutputStream(apk))) {
            zip.putNextEntry(new ZipEntry("AndroidManifest.xml"));zip.write((byte[])encode.invoke(encoder));zip.closeEntry();
        }
        Process p=new ProcessBuilder(args[0],"dump","xmltree",apk.getAbsolutePath(),"--file","AndroidManifest.xml").redirectErrorStream(true).start();
        String xml=new String(p.getInputStream().readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
        if(p.waitFor()!=0)throw new AssertionError(xml);
        if(!xml.contains("foregroundServiceType(0x01010599)="))throw new AssertionError("Missing framework resource ID for service type\n"+xml);
        if(!xml.contains("FOREGROUND_SERVICE_SPECIAL_USE")||!xml.contains("PROPERTY_SPECIAL_USE_FGS_SUBTYPE"))throw new AssertionError("Missing runtime foreground-service declaration");
        for(String service:new String[]{"StubService","InstallService"}) {
            int name=xml.indexOf("org.matonos.compositor.stub."+service);
            int begin=xml.lastIndexOf("E: service",name);
            int end=xml.indexOf("E: service",begin+1);
            String block=xml.substring(begin,end<0?xml.length():end);
            String value=service.equals("StubService")?"1073741824":"1";
            if(!block.contains("foregroundServiceType(0x01010599)="+value+" "))throw new AssertionError("Incorrect service type for "+service+"\n"+block);
        }
        System.out.println("PASS: AOSP parses foreground-service type IDs, install dataSync and runtime specialUse");
    }
}
