package org.matonos.linux.stub;

import java.io.File;
import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.Arrays;
import java.util.Map;
import java.nio.charset.StandardCharsets;
import org.matonos.linuxhost.stubgen.PackLaunch;
import org.matonos.linuxhost.stubgen.DeploymentRecord;

/** Regression for GNOME runtime extensions and their per-image EROFS backing. */
public final class PackageMountsPlanTest {
    private static void check(boolean condition,String message) {
        if(!condition)throw new AssertionError(message);
    }
    private static void target(Map<String,String> targets,String ref,String mount)throws Exception {
        targets.put(ref,PackageMounts.launchTarget(ref,mount));
    }
    private static DeploymentRecord record(String ref,String metadata,DeploymentRecord.Dependency... dependencies)throws Exception {
        byte[] object=("commit:"+ref).getBytes(StandardCharsets.UTF_8);
        String commit=org.matonos.linuxhost.stubgen.ObjectPacks.hex(
                java.security.MessageDigest.getInstance("SHA-256").digest(object));
        return new DeploymentRecord(ref,commit,"flathub",metadata.getBytes(StandardCharsets.UTF_8),new byte[]{1},object,
                Arrays.asList(dependencies),ref.startsWith("app/")?"/app":"/usr");
    }
    private static DeploymentRecord.Dependency dep(String ref,String mount)throws Exception {
        return new DeploymentRecord.Dependency(ref,"flathub",mount);
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

        // One installed freedesktop runtime is /usr for Tux Racer, but only
        // staging metadata for Kate whose own runtime is KDE 6.10.
        String tux="app/com.tuxracer.TuxRacer/x86_64/stable";
        String kate="app/org.kde.kate/x86_64/stable";
        String kde="runtime/org.kde.Platform/x86_64/6.10";
        String kdeLocale="runtime/org.kde.Platform.Locale/x86_64/6.10";
        String fdLocale="runtime/org.freedesktop.Platform.Locale/x86_64/25.08";
        DeploymentRecord fdRecord=record("runtime/org.freedesktop.Platform/x86_64/25.08",
                "[Extension org.freedesktop.Platform.Locale]\ndirectory=share/runtime/locale\n",
                dep(fdLocale,"/usr/share/runtime/locale"));
        DeploymentRecord tuxRecord=record(tux,"[Application]\nruntime=org.freedesktop.Platform/x86_64/25.08\n",
                dep(fdRecord.ref,"/usr"));
        Map<String,String> tuxTargets=PackageMounts.launchTargets(Arrays.asList(tuxRecord,fdRecord,
                record(fdLocale,"[ExtensionOf]\nref="+fdRecord.ref+"\n")));
        check("/usr".equals(tuxTargets.get(fdRecord.ref)),"shared fd runtime must be /usr for its own app");
        check("/usr/share/runtime/locale".equals(tuxTargets.get(fdLocale)),"freedesktop Locale target mismatch");

        DeploymentRecord kateRecord=record(kate,"[Application]\nruntime=org.kde.Platform/x86_64/6.10\n",
                dep(kde,"/usr"));
        DeploymentRecord kdeRecord=record(kde,"[Extension org.kde.Platform.Locale]\ndirectory=share/runtime/locale\n",
                dep(kdeLocale,"/usr/share/runtime/locale"),dep(fdRecord.ref,"/usr"));
        DeploymentRecord kdeLocaleRecord=record(kdeLocale,"[ExtensionOf]\nref="+kde+"\n");
        Map<String,String> kateTargets=PackageMounts.launchTargets(Arrays.asList(kateRecord,kdeRecord,kdeLocaleRecord,fdRecord,
                record(fdLocale,"[ExtensionOf]\nref="+fdRecord.ref+"\n")));
        check("/usr".equals(kateTargets.get(kde)),"Kate's KDE runtime must own /usr");
        check("/usr/share/runtime/locale".equals(kateTargets.get(kdeLocale)),"KDE Locale target mismatch");
        check(kateTargets.get(fdRecord.ref)==null,"shared freedesktop runtime must not claim /usr for Kate");
        check(kateTargets.get(fdLocale)==null,"staging runtime Locale must not claim a virtual target for Kate");
        System.out.println("PASS shared freedesktop runtime target follows the launching app; KDE 6.10 stages it without /usr");

        String gnomeApp="app/org.gnome.TextEditor/x86_64/stable";
        String gnomeAppLocale="runtime/org.gnome.Platform.Locale/x86_64/51";
        DeploymentRecord gnomeAppRecord=record(gnomeApp,"[Application]\nruntime=org.gnome.Platform/x86_64/51\n",dep(gnome,"/usr"));
        DeploymentRecord gnomeRecord=record(gnome,"[Extension org.gnome.Platform.Locale]\ndirectory=share/runtime/locale\n",
                dep(gnomeAppLocale,"/usr/share/runtime/locale"));
        Map<String,String> gnomeTargets=PackageMounts.launchTargets(Arrays.asList(gnomeAppRecord,gnomeRecord,
                record(gnomeAppLocale,"[ExtensionOf]\nref="+gnome+"\n")));
        check("/usr".equals(gnomeTargets.get(gnome)),"GNOME 51 runtime must own /usr");
        check("/usr/share/runtime/locale".equals(gnomeTargets.get(gnomeAppLocale)),"GNOME 51 Locale target mismatch");
        System.out.println("PASS GNOME 51 launch runtime and Locale targets are derived from app metadata");

        String duplicateA="runtime/org.example.ExtensionA/x86_64/stable";
        String duplicateB="runtime/org.example.ExtensionB/x86_64/stable";
        String duplicateRuntime="runtime/org.example.Platform/x86_64/stable";
        DeploymentRecord duplicateApp=record("app/org.example.Duplicate/x86_64/stable",
                "[Application]\nruntime=org.example.Platform/x86_64/stable\n",dep(duplicateRuntime,"/usr"));
        DeploymentRecord duplicateBase=record(duplicateRuntime,
                "[Extension org.example.ExtensionA]\ndirectory=lib/shared\n"+
                "[Extension org.example.ExtensionB]\ndirectory=lib/shared\n",dep(duplicateA,"/usr/lib/shared"),dep(duplicateB,"/usr/lib/shared"));
        try {
            PackageMounts.launchTargets(Arrays.asList(duplicateApp,duplicateBase,
                    record(duplicateA,"[ExtensionOf]\nref="+duplicateRuntime+"\n"),
                    record(duplicateB,"[ExtensionOf]\nref="+duplicateRuntime+"\n")));
            throw new AssertionError("launch plan accepted two packages at one virtual target");
        } catch(IOException expected) {
            check(expected.getMessage().contains(duplicateA)&&expected.getMessage().contains(duplicateB),
                    "duplicate launch target must name both packages: "+expected.getMessage());
        }
        System.out.println("PASS duplicate virtual target fails at launch and names both packages");
        System.out.println("PASS GNOME 51 runtime, Locale, freedesktop GL/codecs targets, staged parent and per-package EROFS backing");
        System.out.println("PASS duplicate virtual target rejected during planning");
    }
}
