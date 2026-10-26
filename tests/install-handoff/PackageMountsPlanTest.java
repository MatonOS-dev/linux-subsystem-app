package org.matonos.linux.stub;

import java.io.File;
import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.Map;
import org.matonos.linuxhost.stubgen.PackLaunch;

/** Regression for GNOME runtime extensions and their per-image EROFS backing. */
public final class PackageMountsPlanTest {
    private static void check(boolean condition,String message) {
        if(!condition)throw new AssertionError(message);
    }
    private static void target(Map<String,String> targets,String ref,String mount)throws Exception {
        targets.put(ref,PackageMounts.launchTarget(ref,mount));
    }
    public static void main(String[] args)throws Exception {
        String gnome="runtime/org.gnome.Platform/x86_64/51";
        String gnomeLocale="runtime/org.gnome.Platform.Locale/x86_64/51";
        String fd="runtime/org.freedesktop.Platform/x86_64/26.08";
        String gl="runtime/org.freedesktop.Platform.GL.default/x86_64/26.08";
        String maton="runtime/org.freedesktop.Platform.GL.maton/x86_64/26.08-extra";
        String codecs="runtime/org.freedesktop.Platform.codecs-extra/x86_64/26.08";
        Map<String,String> metadata=new LinkedHashMap<>();
        metadata.put(gnome,"[Extension org.gnome.Platform.Locale]\ndirectory=share/runtime/locale\n");
        metadata.put(fd,"[Extension org.freedesktop.Platform.GL]\ndirectory=lib/x86_64-linux-gnu/GL\n"+
                "[Extension org.freedesktop.Platform.codecs-extra]\ndirectory=lib/x86_64-linux-gnu/gstreamer-1.0\n");
        String parent="[ExtensionOf]\nref="+fd+"\n";
        Map<String,String> targets=new LinkedHashMap<>();
        target(targets,gnome,"/usr");
        target(targets,gnomeLocale,PackageMounts.mountPoint(gnomeLocale,"[ExtensionOf]\nref="+gnome+"\n",metadata));
        target(targets,gl,PackageMounts.mountPoint(gl,parent,metadata));
        target(targets,maton,PackageMounts.mountPoint(maton,parent,metadata));
        target(targets,codecs,PackageMounts.mountPoint(codecs,parent,metadata));
        targets.put(fd,PackageMounts.stagingTarget(fd));
        check("/usr".equals(targets.get(gnome)),"GNOME runtime must own /usr");
        check("/usr/share/runtime/locale".equals(targets.get(gnomeLocale)),"GNOME Locale target mismatch");
        check("/usr/lib/x86_64-linux-gnu/GL/default".equals(targets.get(gl)),"GL default target mismatch");
        check("/usr/lib/x86_64-linux-gnu/GL/maton".equals(targets.get(maton)),"GL Maton target mismatch");
        check("/usr/lib/x86_64-linux-gnu/gstreamer-1.0".equals(targets.get(codecs)),"codecs target mismatch");
        check(("/usr/.matonos-staging/"+fd).equals(targets.get(fd)),"ExtensionOf parent must stay outside the app runtime targets");
        PackageMounts.validateUniqueTargets(targets);

        // Each target keeps its own package deployment as the physical backing.
        String commit="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        for(Map.Entry<String,String> entry:targets.entrySet()) {
            File files=new File("/deployment/system/"+entry.getKey()+"/"+commit+"/files");
            java.util.List<String> backing=PackLaunch.backingArguments(entry.getValue(),files);
            check(backing.get(1).equals(entry.getValue()),"backing target was reassigned for "+entry.getKey());
            check(backing.get(2).equals(files.getAbsolutePath()),"backing files were shared for "+entry.getKey());
        }

        Map<String,String> duplicate=new LinkedHashMap<>(targets);
        duplicate.put("runtime/org.freedesktop.Platform/x86_64/26.08","/usr");
        try {PackageMounts.validateUniqueTargets(duplicate);throw new AssertionError("duplicate target accepted");}
        catch(IOException expected) {check(expected.getMessage().contains("Duplicate package launch target /usr"),"unclear duplicate error");}
        System.out.println("PASS GNOME 51 runtime, Locale, freedesktop GL/codecs targets, staged parent and per-package EROFS backing");
        System.out.println("PASS duplicate virtual target rejected during planning");
    }
}
