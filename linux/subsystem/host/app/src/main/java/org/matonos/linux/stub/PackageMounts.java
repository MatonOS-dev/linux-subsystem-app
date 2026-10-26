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
    /** Apply MatonOS's split view for the stock GL extension mount. */
    public static String launchTarget(String ref,String target) throws IOException {
        if(target==null)throw new IOException("Package has no launch target: "+ref);
        String branch=ref.startsWith("runtime/")?ref.substring(ref.lastIndexOf('/')+1):"";
        if(target.endsWith("/GL")&&ref.startsWith("runtime/org.freedesktop.Platform.GL."))
            return target+(branch.endsWith("-extra")?"/maton":"/default");
        return target;
    }
    /** A staged ExtensionOf parent has no app-visible mount in this launch. */
    public static String stagingTarget(String ref) throws IOException {
        if(!DeployManifest.validRef(ref))throw new IOException("Invalid staged-only package ref: "+ref);
        return "/usr/.matonos-staging/"+ref;
    }
    /** Reject ambiguous launch plans before pulling packages or starting bwrap. */
    public static void validateUniqueTargets(java.util.Map<String,String> packageTargets) throws IOException {
        java.util.Map<String,String> owners=new java.util.HashMap<>();
        for(java.util.Map.Entry<String,String> entry:packageTargets.entrySet()) {
            String ref=entry.getKey(),target=entry.getValue();
            if(target==null||!target.matches("/(?:usr|app)(?:/[A-Za-z0-9_.+-]+)*"))
                throw new IOException("Invalid launch target for "+ref+": "+target);
            String previous=owners.putIfAbsent(target,ref);
            if(previous!=null&&!previous.equals(ref))
                throw new IOException("Duplicate package launch target "+target+": "+previous+" and "+ref);
        }
    }
    /** Select the runtime's branch when supported, otherwise its first GL extension version. */
    public static String glExtensionBranch(String runtimeMetadata,String runtimeBranch) throws IOException {
        if(runtimeMetadata==null)throw new IOException("Runtime has no org.freedesktop.Platform.GL extension point");
        if(runtimeBranch==null||!runtimeBranch.matches("[A-Za-z0-9_.+-]+"))throw new IOException("Invalid Maton GL extension branch");
        String versions=value(runtimeMetadata,"Extension org.freedesktop.Platform.GL","versions");
        if(versions==null)versions=value(runtimeMetadata,"Extension org.freedesktop.Platform.GL","version");
        if(versions==null)throw new IOException("Runtime has no org.freedesktop.Platform.GL extension point");
        String first=null;
        for(String version:versions.split(";")) {
            String candidate=version.trim();
            if(candidate.isEmpty())continue;
            if(!candidate.matches("[A-Za-z0-9_.+-]+"))throw new IOException("Invalid runtime GL extension version");
            if(first==null)first=candidate;
            if(runtimeBranch.equals(candidate))return candidate;
        }
        if(first==null)throw new IOException("Runtime GL extension point has no versions");
        return first;
    }
    public static String extensionParent(String metadata) throws IOException {
        String value=value(metadata,"ExtensionOf","ref");if(value==null)return null;
        if(!DeployManifest.validRef(value)||(!value.startsWith("runtime/")&&!value.startsWith("app/")))throw new IOException("Invalid Flatpak extension parent ref");return value;
    }
    /** Metadata for extensions may belong to an already-installed parent omitted from the plan. */
    static java.util.Map<String,String> metadataWithInstalled(java.util.Map<String,String> planned,
            java.util.Map<String,String> installed) {
        java.util.Map<String,String> result=new java.util.HashMap<>(planned);
        for(java.util.Map.Entry<String,String> entry:installed.entrySet())result.putIfAbsent(entry.getKey(),entry.getValue());
        return result;
    }
    public static String mountPoint(String ref,String metadata,java.util.Map<String,String> parentMetadata) throws IOException {
        String parent=extensionParent(metadata);if(parent==null){if(ref.startsWith("runtime/"))return "/usr";throw new IOException("Application deployment is not a Flatpak extension: "+ref);}
        String parentText=parentMetadata.get(parent);
        if(parentText==null) {
            Parent resolved=plannedX86_64Parent(ref,extensionId(ref),parent,parentMetadata);
            if(resolved!=null){parent=resolved.ref;parentText=resolved.metadata;}
        }
        if(parentText==null)throw new IOException("Extension parent deployment is missing: "+parent);
        String id=extensionId(ref);String dir=extensionDirectory(parentText,id);if(dir==null)throw new IOException("Flatpak metadata has no directory for extension "+id);return (parent.startsWith("app/")?"/app/":"/usr/")+dir;
    }
    /**
     * Freedesktop's x86_64 Compat.i386 and GL32 refs can declare an i386
     * ExtensionOf.ref even though the installable parent is the already
     * planned x86_64 runtime whose metadata declares that extension point.
     * Resolve only against staged refs; never synthesize a replacement ref.
     */
    private static final class Parent {
        final String ref,metadata;
        Parent(String ref,String metadata){this.ref=ref;this.metadata=metadata;}
    }
    private static Parent plannedX86_64Parent(String extension,String id,String missingParent,
            java.util.Map<String,String> metadata) throws IOException {
        String[] extensionParts=extension.split("/",-1),parentParts=missingParent.split("/",-1);
        if(extensionParts.length!=4||!extensionParts[0].equals("runtime")||!extensionParts[2].equals("x86_64")||
                parentParts.length!=4||!parentParts[0].equals("runtime")||
                !parentParts[1].equals("org.freedesktop.Platform")||!parentParts[2].equals("i386"))return null;
        Parent match=null;
        for(java.util.Map.Entry<String,String> entry:metadata.entrySet()) {
            String[] candidate=entry.getKey().split("/",-1);
            if(extensionDirectory(entry.getValue(),id)==null)continue;
            String runtimeRef=entry.getKey();
            if(candidate.length==4&&candidate[0].equals("app")) {
                String declared=value(entry.getValue(),"Application","runtime");
                if(declared==null)continue;
                String staged="runtime/"+declared;
                if(!metadata.containsKey(staged))continue;
                runtimeRef=staged;
                candidate=runtimeRef.split("/",-1);
            }
            if(candidate.length!=4||!candidate[0].equals("runtime")||!candidate[2].equals("x86_64"))continue;
            Parent found=new Parent(runtimeRef,entry.getValue());
            if(match!=null&&!match.ref.equals(found.ref))
                throw new IOException("Ambiguous staged x86_64 parent for extension "+id);
            match=found;
        }
        return match;
    }
    static String extensionDirectory(String metadata,String id) throws IOException {
        for(String candidate=id;;) {
            String value=value(metadata,"Extension "+candidate,"directory");
            if(value!=null){validateDirectory(value);return value;}
            int dot=candidate.lastIndexOf('.');if(dot<1)return null;candidate=candidate.substring(0,dot);
        }
    }
    private static void validateDirectory(String value) throws IOException {
        if(value.isEmpty()||value.startsWith("/")||value.indexOf('\\')>=0)throw new IOException("Invalid Flatpak extension directory");
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
