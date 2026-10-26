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
        Class<?> manifest=Class.forName("org.matonos.linuxhost.stubgen.StubGenerator$BinaryManifest");
        Constructor<?> ctor=manifest.getDeclaredConstructor(String.class,String.class,String.class,String.class,java.util.List.class);
        ctor.setAccessible(true);
        Object encoder=ctor.newInstance("flatpak.org.example.Test","Test","app/org.example.Test/x86_64/stable","flathub",Collections.emptyList());
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
            int name=xml.indexOf("org.matonos.linux.stub."+service);
            int begin=xml.lastIndexOf("E: service",name);
            int end=xml.indexOf("E: service",begin+1);
            String block=xml.substring(begin,end<0?xml.length():end);
            String value=service.equals("StubService")?"1073741824":"1";
            if(!block.contains("foregroundServiceType(0x01010599)="+value+" "))throw new AssertionError("Incorrect service type for "+service+"\n"+block);
        }
        // The host loader replaces the shared-library class loader; the ABI
        // marker and extractNativeLibs are gone with it.
        if(!xml.contains("appComponentFactory(0x0101057a)=\"org.matonos.linux.stub.StubLoader\""))throw new AssertionError("Missing host StubLoader factory\n"+xml);
        if(xml.contains("extractNativeLibs"))throw new AssertionError("extractNativeLibs must not be set\n"+xml);
        if(xml.contains("uses-library"))throw new AssertionError("Stubs must not declare a host uses-library\n"+xml);
        if (xml.contains("org.matonos.systembridge"))
            throw new AssertionError("Generated stubs no longer query the system bridge\n"+xml);
        if (xml.contains("org.matonos.permission.SYSTEM_BRIDGE"))
            throw new AssertionError("A generated stub must not request the general bridge permission");
        if (!xml.contains("authorities(0x01010018)=\"flatpak.org.example.Test.portal.files\"")
                || !xml.contains("grantUriPermissions(0x0101001b)="))
            throw new AssertionError("Missing per-stub portal provider capability declaration\n"+xml);
        if (xml.contains("org.matonos.linuxruntimes"))
            throw new AssertionError("Retired runtime package query present\n"+xml);
        Object dependency=ctor.newInstance("flatpak.runtime.org.example.Platform.x86_64.stable.xabcdef123456","","runtime/org.example.Platform/x86_64/stable","example",Collections.emptyList());
        java.lang.reflect.Field noActivities=manifest.getDeclaredField("noActivities");noActivities.setAccessible(true);noActivities.setBoolean(dependency,true);
        java.lang.reflect.Field mount=manifest.getDeclaredField("packs");mount.setAccessible(true);mount.set(dependency,"/usr");
        java.lang.reflect.Field declared=manifest.getDeclaredField("dependencyPackages");declared.setAccessible(true);declared.set(dependency,"runtime/org.example.GL/x86_64/stable>flatpak.runtime.GL.xabc>/usr/lib/GL");
        try(ZipOutputStream zip=new ZipOutputStream(new FileOutputStream(apk))) {
            zip.putNextEntry(new ZipEntry("AndroidManifest.xml"));zip.write((byte[])encode.invoke(dependency));zip.closeEntry();
        }
        p=new ProcessBuilder(args[0],"dump","xmltree",apk.getAbsolutePath(),"--file","AndroidManifest.xml").redirectErrorStream(true).start();
        xml=new String(p.getInputStream().readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
        if(p.waitFor()!=0||xml.contains("E: activity")||!xml.contains("/usr/lib/GL")||!xml.contains("runtime/org.example.Platform"))throw new AssertionError("Nonlaunchable package declaration failed\n"+xml);
        System.out.println("PASS: AOSP parses foreground-service type IDs, install dataSync, runtime specialUse and the host StubLoader factory");
    }
}
