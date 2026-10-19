package org.matonos.linux.stub;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Maps Flatpak extension refs using the authenticated metadata already staged by Flatpak. */
public final class PackageMounts {
    public interface Metadata { String read(String ref,String commit) throws Exception; }
    public static final class Entry { public final String ref,packageName,mountPoint;
        Entry(String r,String p,String m){ref=r;packageName=p;mountPoint=m;} }
    public static List<Entry> resolve(List<String> refs,List<String> commits,String appRef,Metadata metadata) throws Exception {
        if(refs==null||commits==null||refs.size()!=commits.size())throw new IOException("Inconsistent Flatpak deployment list");
        List<Entry> result=new ArrayList<>();
        java.util.Map<String,String> allMetadata=new java.util.HashMap<>();
        for(int i=0;i<refs.size();i++)allMetadata.put(refs.get(i),metadata.read(refs.get(i),commits.get(i)));
        for(int i=0;i<refs.size();i++) {
            String ref=refs.get(i);if(ref==null||ref.equals(appRef))continue;
            if(!ref.startsWith("runtime/")&&!ref.startsWith("app/"))throw new IOException("Invalid Flatpak runtime or extension ref");
            String mount=mountPoint(ref,allMetadata.get(ref),allMetadata);
            result.add(new Entry(ref,DeployManifest.packageForPublic(ref),mount));
        }
        return Collections.unmodifiableList(result);
    }
    public static Entry entry(String ref,String mountPoint) throws Exception {
        if(mountPoint==null||!mountPoint.matches("/(?:usr|app)(?:/[A-Za-z0-9_.+-]+)*"))throw new IOException("Invalid resolved runtime mount point");
        return new Entry(ref,DeployManifest.packageForPublic(ref),mountPoint);
    }
    public static void requireGlExtensionVersion(String runtimeMetadata,String extensionBranch) throws IOException {
        if(extensionBranch==null||!extensionBranch.matches("[A-Za-z0-9_.+-]+"))throw new IOException("Invalid Maton GL extension branch");
        String versions=value(runtimeMetadata,"Extension org.freedesktop.Platform.GL","versions");
        if(versions==null)versions=value(runtimeMetadata,"Extension org.freedesktop.Platform.GL","version");
        if(versions==null)throw new IOException("Runtime has no org.freedesktop.Platform.GL extension point");
        for(String version:versions.split(";"))if(extensionBranch.equals(version.trim()))return;
        throw new IOException("Maton GL extension branch "+extensionBranch+" is not supported by the runtime GL extension point");
    }
    public static String extensionParent(String metadata) throws IOException {
        String value=value(metadata,"ExtensionOf","ref");if(value==null)return null;
        if(!DeployManifest.validRef(value)||(!value.startsWith("runtime/")&&!value.startsWith("app/")))throw new IOException("Invalid Flatpak extension parent ref");return value;
    }
    public static String mountPoint(String ref,String metadata,java.util.Map<String,String> parentMetadata) throws IOException {
        String parent=extensionParent(metadata);if(parent==null){if(ref.startsWith("runtime/"))return "/usr";throw new IOException("Application deployment is not a Flatpak extension: "+ref);}
        String parentText=parentMetadata.get(parent);if(parentText==null)throw new IOException("Extension parent deployment is missing: "+parent);
        String id=extensionId(ref);String dir=extensionDirectory(parentText,id);if(dir==null)throw new IOException("Flatpak metadata has no directory for extension "+id);return (parent.startsWith("app/")?"/app/":"/usr/")+dir;
    }
    static String extensionDirectory(String metadata,String id) throws IOException {
        for(String candidate=id;;) {
            String value=value(metadata,"Extension "+candidate,"directory");
            if(value!=null){validateDirectory(value);return value;}
            int dot=candidate.lastIndexOf('.');if(dot<1)return null;candidate=candidate.substring(0,dot);
        }
    }
    private static void validateDirectory(String value) throws IOException {
        if(value.isEmpty()||value.startsWith("/")||value.length()>1024||value.indexOf('\\')>=0)throw new IOException("Invalid Flatpak extension directory");
        for(String part:value.split("/",-1))if(part.isEmpty()||part.equals(".")||part.equals(".."))throw new IOException("Invalid Flatpak extension directory");
    }
    private static String value(String text,String section,String key) throws IOException {
        String wanted="["+section+"]";boolean inside=false;String found=null;
        for(String line:text.split("\\r?\\n",-1)) {
            String trimmed=line.trim();if(trimmed.startsWith("[")){inside=trimmed.equals(wanted);continue;}
            int equals=trimmed.indexOf('=');
            if(inside&&equals>=0&&trimmed.substring(0,equals).trim().equals(key)){
                if(found!=null)throw new IOException("Duplicate Flatpak metadata key "+key);
                found=trimmed.substring(equals+1).trim();
            }
        }
        return found;
    }
    private static String extensionId(String ref) throws IOException {
        String[] parts=ref.split("/",-1);if(parts.length!=4||(!parts[0].equals("runtime")&&!parts[0].equals("app")))throw new IOException("Invalid runtime or extension ref");return parts[1];
    }
    private PackageMounts(){}
}
