package org.matonos.linuxhost.stubgen;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;

import com.android.apksig.ApkSigner;

import java.io.ByteArrayOutputStream;
import java.io.BufferedReader;
import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FilterOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.StringReader;
import java.nio.charset.StandardCharsets;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Creates a signed, code-free launcher APK for one installed Flatpak ref.
 * App stubs declare the OSTree commit checksums of the app and its runtime
 * in manifest meta-data, replacing the retired erofs image entries. */
public final class StubGenerator {
    /** Host-test seam for the Android Keystore boundary; unset in production. */
    interface SigningBackend { void sign(File input, File output) throws Exception; }
    private static volatile SigningBackend testSigningBackend;
    static void setSigningBackendForTest(SigningBackend backend) { testSigningBackend = backend; }
    public static final int FORMAT_VERSION = 1;
    // v14 adds bridge visibility for self-task Linux window capabilities.
    // v19 moves the launcher entry into a code-free launcher split, applies
    // Flatpak extra data in a per-package service, and carries the result in a
    // self-contained extra split mounted over the package's extra directory.
    public static final int STUB_VERSION_CODE = 21;
    public static final String HOST_ACTIVITY = "org.matonos.linux.stub.StubActivity";
    public static final String HOST_SERVICE = "org.matonos.linux.stub.StubService";
    /** Per-package service that applies this package's Flatpak extra data. */
    public static final String EXTRA_SERVICE = "org.matonos.linux.stub.ExtraService";
    /** Mount point the extra service mounts this package's packs at. */
    public static final String EXTRA_MOUNT_META = "org.matonos.linuxhost.EXTRA_MOUNT";
    /** Split carrying the launcher components (activity-alias) and nothing else. */
    public static final String LAUNCHER_SPLIT_PREFIX = "launcher_";
    /** Split carrying the applied extra output (and possibly the launcher). */
    public static final String EXTRA_SPLIT_PREFIX = "extra_";
    /** The mount point an app's extra directory lives under. */
    public static final String APP_MOUNT = "/app";
    /** Host-side AppComponentFactory: loads the stub's components from the current host APK. */
    public static final String HOST_LOADER = "org.matonos.linux.stub.StubLoader";
    /** Unprivileged entry point (the store uses it): asks the bridge to install this stub's Flatpak; no-op when installed. */
    public static final String INSTALL_ACTIVITY = "org.matonos.linux.stub.InstallActivity";
    /** Action the store resolves (by its FLATPAK_REF metadata) to find a new stub's install activity. */
    public static final String INSTALL_ACTION = "org.matonos.linuxhost.INSTALL";
    /** "Update me": same lookup as INSTALL_ACTION. */
    public static final String UPDATE_ACTIVITY = "org.matonos.linux.stub.UpdateActivity";
    public static final String UPDATE_ACTION = "org.matonos.linuxhost.UPDATE";
    public static final String REF_META = "org.matonos.linuxhost.FLATPAK_REF";
    public static final String REMOTE_META = "org.matonos.linuxhost.FLATPAK_REMOTE";
    public static final String MIN_INTERFACE_META = "org.matonos.linuxhost.MIN_INTERFACE_VERSION";
    /** App OSTree commit checksum (64 lowercase hex chars). */
    public static final String APP_COMMIT_META = "org.matonos.linuxhost.APP_COMMIT";
    /** Runtime Flatpak ref, e.g. runtime/org.freedesktop.Platform/x86_64/stable. */
    public static final String RUNTIME_REF_META = "org.matonos.linuxhost.RUNTIME_REF";
    /** Runtime OSTree commit checksum (64 lowercase hex chars). */
    public static final String RUNTIME_COMMIT_META = "org.matonos.linuxhost.RUNTIME_COMMIT";
    public static final String REQUIRED_PACKAGES_META = "org.matonos.linuxhost.REQUIRED_PACKAGES";
    public static final String PACKS_META = "org.matonos.linuxhost.PACKS";
    public static final String DEPENDENCIES_META = "org.matonos.linuxhost.DEPENDENCIES";
    public static final String APP_REF_META = "org.matonos.linuxhost.APP_REF";
    private static final String KEY_ALIAS = "org.matonos.linux.flatpak_stub_v1";
    private static final int MAX_ICON_BYTES = 1024 * 1024;

    public static final String GAME_CONTROLLERS = "org.matonos.permission.GAME_CONTROLLERS";

    /** Validate a 64-char hex commit checksum (lowercase). */
    public static boolean validCommit(String commit) {
        return commit != null && commit.length() == 64 && commit.matches("[0-9a-f]{64}");
    }

    /** Read only the declared Context devices, never app IDs or overrides. */
    public static List<String> permissionsForMetadata(String metadata) throws IOException {
        boolean context = false;
        String devices = "";
        try (BufferedReader reader = new BufferedReader(new StringReader(metadata))) {
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (line.startsWith("#") || line.startsWith(";")) continue;
                if (line.startsWith("[")) { context = line.equals("[Context]"); continue; }
                int equals = line.indexOf('=');
                if (context && equals >= 0) {
                    String key = line.substring(0, equals).trim();
                    if (key.equals("devices")) devices = line.substring(equals + 1).trim();
                }
            }
        }
        List<String> permissions = new ArrayList<>();
        for (String device : devices.split(";"))
            if (device.trim().equals("all") || device.trim().equals("input")) {
                permissions.add(GAME_CONTROLLERS);
                break;
            }
        return permissions;
    }

    private StubGenerator() { }

    /** Container for OSTree commit information carried in a stub's manifest. */
    public static final class CommitInfo {
        public final String appCommit;
        public final String runtimeRef;
        public final String runtimeCommit;
        public CommitInfo(String appCommit, String runtimeRef, String runtimeCommit) {
            if (!validCommit(appCommit))
                throw new IllegalArgumentException("Invalid app commit: must be 64 lowercase hex chars");
            if (!validRef(runtimeRef) || !runtimeRef.startsWith("runtime/"))
                throw new IllegalArgumentException("Invalid runtime ref");
            if (!validCommit(runtimeCommit))
                throw new IllegalArgumentException("Invalid runtime commit: must be 64 lowercase hex chars");
            this.appCommit = appCommit;
            this.runtimeRef = runtimeRef;
            this.runtimeCommit = runtimeCommit;
        }
    }

    /**
     * @param workDir private scratch directory owned by the calling app
     * @param ref installed Flatpak ref, e.g. app/org.example.Editor/x86_64/stable
     * @param label launcher label from the remote's appstream entry for the ref
     * @param iconPng PNG icon bytes selected by the caller
     * @param permissions Android permission names approved for this stub
     * @param packageName unique Android package name chosen by the store
     * @param output destination APK
     * @param commitInfo app OSTree commit checksum, runtime ref and runtime commit checksum
     */
    public static File generate(File workDir, String ref, String label, byte[] iconPng,
            List<String> permissions, String packageName, File output, CommitInfo commitInfo) throws Exception {
        return generate(workDir, ref, label, iconPng, permissions, packageName, output, commitInfo, null);
    }

    /**
     * @param workDir private scratch directory owned by the calling app
     * @param ref installed Flatpak ref, e.g. app/org.example.Editor/x86_64/stable
     * @param label launcher label from the remote's appstream entry for the ref
     * @param iconPng PNG icon bytes selected by the caller
     * @param permissions Android permission names approved for this stub
     * @param packageName unique Android package name chosen by the store
     * @param output destination APK
     * @param commitInfo app OSTree commit checksum with runtime ref/commit, or null for no-image stubs
     * @param minInterfaceVersion minimum interface version (null defaults to 2)
     */
    public static File generate(File workDir, String ref, String label, byte[] iconPng,
            List<String> permissions, String packageName, File output, CommitInfo commitInfo,
            Integer minInterfaceVersion) throws Exception {
        return generate(workDir, ref, "flathub", label, iconPng, permissions,
                packageName, output, commitInfo, minInterfaceVersion);
    }

    public static File generate(File workDir, String ref, String remote, String label,
            byte[] iconPng, List<String> permissions, String packageName, File output,
            CommitInfo commitInfo, Integer minInterfaceVersion) throws Exception {
        return generate(workDir,ref,remote,label,iconPng,permissions,packageName,output,
                commitInfo,minInterfaceVersion,null);
    }

    public static File generateWithPackages(File workDir, String ref, String remote, String label,
            byte[] iconPng, List<String> permissions, String packageName, File output,
            Integer minInterfaceVersion, File packDirectory, List<String> requiredPackages) throws Exception {
        return generateWithDependencies(workDir,ref,remote,label,iconPng,permissions,packageName,output,
                minInterfaceVersion,packDirectory,requiredPackages,Collections.emptyMap());
    }
    public static File generateWithDependencies(File workDir,String ref,String remote,String label,
            byte[] iconPng,List<String> permissions,String packageName,File output,Integer minInterfaceVersion,
            File packDirectory,List<String> requiredPackages,Map<String,String> dependencyPackages) throws Exception {
        return generatePackage(workDir,ref,remote,label,iconPng,permissions,packageName,output,
                packDirectory,requiredPackages,dependencyPackages,true,"/app");
    }
    public static File generatePackage(File workDir,String ref,String remote,String label,byte[] iconPng,
            List<String> permissions,String packageName,File output,File packDirectory,List<String> requiredPackages,
            Map<String,String> dependencies,boolean launchable,String mountPoint) throws Exception {
        if(!validRef(ref)||(packageName==null||!packageName.matches("[A-Za-z][A-Za-z0-9_.]+")))throw new IllegalArgumentException("Invalid package identity");
        if(!launchable&&(mountPoint==null||!mountPoint.matches("/(?:usr|app)(?:/[A-Za-z0-9_.+-]+)*")||Arrays.asList(mountPoint.split("/")).contains("..")))throw new IllegalArgumentException("Invalid package mount");
        BinaryManifest manifest=new BinaryManifest(packageName,cleanLabel(label),ref,remote,permissions);
        manifest.minInterfaceVersion=2;manifest.noActivities=!launchable;
        manifest.packs=launchable?String.join(";",requiredPackages):mountPoint;
        manifest.dependencyPackages=encodeDependencies(dependencies);
        File unsigned=File.createTempFile("matonos-stub-meta-",".apk",workDir);
        try {
            if(launchable)writeUnsigned(unsigned,packageName,cleanLabel(label),ref,remote,iconPng,permissions,null,2,packDirectory,manifest.packs,manifest.dependencyPackages);
            else try(AlignedZipOutputStream zip=new AlignedZipOutputStream(new FileOutputStream(unsigned))) {
                put(zip,"AndroidManifest.xml",manifest.encode());put(zip,"classes.dex",loaderDex());
            }
            sign(unsigned,output);return output;
        } finally {unsigned.delete();}
    }
    private static String encodeDependencies(Map<String,String> values){StringBuilder out=new StringBuilder();for(Map.Entry<String,String> e:new java.util.TreeMap<>(values).entrySet()){if(out.length()>0)out.append(';');out.append(e.getKey()).append('>').append(e.getValue());}return out.toString();}

    /** Pack-enabled base; only the small index/metadata live here, payloads are splits. */
    public static File generate(File workDir, String ref, String remote, String label,
            byte[] iconPng, List<String> permissions, String packageName, File output,
            CommitInfo commitInfo, Integer minInterfaceVersion, File packDirectory) throws Exception {
        if (packDirectory != null && (!new File(packDirectory,"objects.index").isFile()
                || !new File(packDirectory,"objects.ostree").isFile()))
            throw new IOException("Missing pack index or metadata");
        if (workDir == null)
            throw new IllegalArgumentException("Private work directory is required");
        if (iconPng == null || iconPng.length == 0 || iconPng.length > MAX_ICON_BYTES) throw new IllegalArgumentException("PNG icon size is invalid");
        if (!validRef(ref))
            throw new IllegalArgumentException("Invalid Flatpak ref");
        if (remote == null || remote.length() > 128 || !remote.matches("[A-Za-z0-9._-]+"))
            throw new IllegalArgumentException("Invalid Flatpak remote");
        if (packageName == null || packageName.length() > 255 || !packageName.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+"))
            throw new IllegalArgumentException("Invalid stub package name");
        if (output == null) throw new IllegalArgumentException("Output APK is required");

        String cleanLabel = cleanLabel(label);
        File parent = output.getAbsoluteFile().getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create output directory");
        if (!workDir.isDirectory() && !workDir.mkdirs()) throw new IOException("Cannot create private work directory");
        File unsigned = File.createTempFile("matonos-stub-", ".unsigned.apk", workDir);
        try {
            writeUnsigned(unsigned, packageName, cleanLabel, ref, remote, iconPng, permissions, commitInfo,
                    minInterfaceVersion != null ? minInterfaceVersion : 2, packDirectory);
            sign(unsigned, output);
            return output;
        } finally {
            //noinspection ResultOfMethodCallIgnored
            unsigned.delete();
        }
    }

    private static List<String> indexAssets(File objects) {
        List<String> names=new ArrayList<>(Arrays.asList("objects.index","objects.ostree"));
        if(new File(objects,"deployment.bin").isFile())names.add("deployment.bin");
        if(new File(objects,"original.ostree").isFile()){names.add("original.ostree");names.add("original.index");}
        return names;
    }
    /** Index identity includes metadata; both are immutable in this split. */
    public static String indexSplitName(File objects) throws Exception {
        java.security.MessageDigest hash=java.security.MessageDigest.getInstance("SHA-256");
        for(String name:indexAssets(objects)) {
            byte[] bytes=java.nio.file.Files.readAllBytes(new File(objects,name).toPath());
            hash.update(java.nio.ByteBuffer.allocate(8).putLong(bytes.length).array());hash.update(bytes);
        }
        return "index_"+ObjectPacks.hex(hash.digest());
    }
    public static File generateIndex(File workDir,String pkg,File objects,File output,int revision) throws Exception {
        if(revision<0)throw new IllegalArgumentException("Negative split revision");
        File unsigned=File.createTempFile("matonos-index-",".apk",workDir);
        try {
            writeIndexUnsigned(unsigned,pkg,objects,revision);
            sign(unsigned,output);return output;
        } finally {unsigned.delete();}
    }
    private static void writeIndexUnsigned(File unsigned,String pkg,File objects,int revision) throws Exception {
        BinaryManifest encoder=new BinaryManifest(pkg,"","","flathub",Collections.emptyList());
        encoder.revisionCode=revision;
        try(AlignedZipOutputStream zip=new AlignedZipOutputStream(new FileOutputStream(unsigned))) {
            put(zip,"AndroidManifest.xml",encoder.packManifest(indexSplitName(objects)));
            for(String name:indexAssets(objects))
                put(zip,"assets/matonos/"+name,java.nio.file.Files.readAllBytes(new File(objects,name).toPath()));
        }
    }

    /** Builds a code-free split named for the immutable content ZIP hash.
     * Indexed offsets survive manifest insertion and v2/v3 signing. */
    public static File generatePack(File workDir, String packageName, File pack, File output) throws Exception {
        return generatePack(workDir,packageName,pack,output,0);
    }
    public static File generatePack(File workDir, String packageName, File pack, File output,int revision) throws Exception {
        if(revision<0)throw new IllegalArgumentException("Negative split revision");
        if(!packageName.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+"))
            throw new IllegalArgumentException("Invalid package name");
        String name=pack.getName();
        if(!name.matches("pack-[0-9a-f]{64}\\.zip"))throw new IllegalArgumentException("Invalid pack name");
        String split="pack_"+name.substring(5,69);
        BinaryManifest encoder=new BinaryManifest(packageName,"","","flathub",Collections.emptyList());
        encoder.revisionCode=revision;
        File unsigned=File.createTempFile("matonos-pack-",".apk",workDir);
        try {
            ObjectPacks.carrier(pack,unsigned,encoder.packManifest(split));
            sign(unsigned,output);return output;
        } finally {unsigned.delete();}
    }

    /** Builds a signed split carrying one stored, page-aligned EROFS deployment.
     * The split metadata is inside the signed APK and lets mountd locate the
     * bytes from the ZIP local header without extracting the image. */
    public static File generateErofsPack(File workDir, String packageName, File image,
            File output, int revision) throws Exception {
        File unsigned=generateErofsPackUnsigned(workDir,packageName,image,revision);
        try {sign(unsigned,output);return output;}
        finally {unsigned.delete();}
    }

    /** Split name for a prepared image, identical to the signed carrier identity. */
    public static String erofsSplitName(File image) throws Exception {
        if(!image.isFile()||image.length()==0)throw new IOException("Missing EROFS image");
        return "erofs_"+fileSha256(image);
    }

    /** Writes the EROFS split before APK signing; used by host signing tests. */
    public static File generateErofsPackUnsigned(File workDir, String packageName, File image,
            int revision) throws Exception {
        if(revision<0)throw new IllegalArgumentException("Negative split revision");
        if(!packageName.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+"))
            throw new IllegalArgumentException("Invalid package name");
        if(!image.isFile() || image.length()==0)throw new IOException("Missing EROFS image");
        String digest=fileSha256(image);
        String split="erofs_"+digest;
        String entry="assets/matonos/deployment.erofs";
        byte[] metadata=("{\"format\":\"matonos-erofs-pack-v1\",\"image_entry\":\""+entry+
                "\",\"image_size\":"+image.length()+",\"image_sha256\":\""+digest+
                "\",\"offset_rule\":\"zip-local-header-v1\",\"offset_entry\":\""+entry+
                "\",\"compression\":\"lz4hc\"}\n")
                .getBytes(StandardCharsets.UTF_8);
        BinaryManifest encoder=new BinaryManifest(packageName,"","","flathub",Collections.emptyList());
        encoder.revisionCode=revision;
        File unsigned=File.createTempFile("matonos-erofs-",".apk",workDir);
        try(AlignedZipOutputStream zip=new AlignedZipOutputStream(new FileOutputStream(unsigned))) {
            put(zip,"AndroidManifest.xml",encoder.packManifest(split));
            put(zip,"assets/matonos/erofs-pack.json",metadata);
            putStoredAligned(zip,entry,image,4096);
        } catch(Exception error) {unsigned.delete();throw error;}
        if(image.length()<=0 || !digest.equals(fileSha256(image))) {
            unsigned.delete();throw new IOException("EROFS image changed while creating split");
        }
        return unsigned;
    }

    /** Signs one complete per-pack EROFS image for an MTPKIDX2 layer table. */
    public static File generateErofsLayerPack(File workDir,String packageName,File image,
            File output,int revision)throws Exception {
        File unsigned=generateErofsLayerPackUnsigned(workDir,packageName,image,revision);
        try{sign(unsigned,output);return output;}finally{unsigned.delete();}
    }

    /** Unsigned MTPKIDX2 carrier; layers.index names this split and asset. */
    public static File generateErofsLayerPackUnsigned(File workDir,String packageName,File image,
            int revision)throws Exception {
        if(revision<0)throw new IllegalArgumentException("Negative split revision");
        if(!packageName.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+"))
            throw new IllegalArgumentException("Invalid package name");
        if(!image.isFile()||image.length()==0||!image.getName().matches("pack-[0-9a-f]{64}\\.erofs"))
            throw new IOException("Invalid EROFS layer image");
        String digest=fileSha256(image),identity=image.getName().substring(5,69);
        if(!digest.equals(identity))throw new IOException("EROFS layer filename hash mismatch");
        String split="pack_"+digest,entry="assets/matonos/pack.erofs";
        BinaryManifest encoder=new BinaryManifest(packageName,"","","flathub",Collections.emptyList());
        encoder.revisionCode=revision;
        File unsigned=File.createTempFile("matonos-layer-",".apk",workDir);
        try(AlignedZipOutputStream zip=new AlignedZipOutputStream(new FileOutputStream(unsigned))) {
            put(zip,"AndroidManifest.xml",encoder.packManifest(split));putStoredAligned(zip,entry,image,4096);
        } catch(Exception error){unsigned.delete();throw error;}
        if(!digest.equals(fileSha256(image))){unsigned.delete();throw new IOException("EROFS layer changed while creating split");}
        return unsigned;
    }

    /** Writes/signs a code-free base split containing the merged-view layers.index. */
    public static File generateLayersIndex(File workDir,String packageName,File index,File output,
            int revision)throws Exception {
        if(revision<0)throw new IllegalArgumentException("Negative split revision");
        if(!index.isFile()||index.length()<12)throw new IOException("Missing layers.index");
        byte[] bytes=java.nio.file.Files.readAllBytes(index.toPath());
        if(!Arrays.equals(Arrays.copyOf(bytes,8),"MTPKIDX2".getBytes(StandardCharsets.US_ASCII)))
            throw new IOException("Invalid layers.index format");
        String split="index_"+fileSha256(index);
        BinaryManifest encoder=new BinaryManifest(packageName,"","","flathub",Collections.emptyList());
        encoder.revisionCode=revision;
        File unsigned=File.createTempFile("matonos-layers-index-",".apk",workDir);
        try(AlignedZipOutputStream zip=new AlignedZipOutputStream(new FileOutputStream(unsigned))) {
            put(zip,"AndroidManifest.xml",encoder.packManifest(split));put(zip,"assets/matonos/layers.index",bytes);
        } catch(Exception error){unsigned.delete();throw error;}
        try{sign(unsigned,output);return output;}finally{unsigned.delete();}
    }

    private static String fileSha256(File file) throws Exception {
        java.security.MessageDigest digest=java.security.MessageDigest.getInstance("SHA-256");
        try(InputStream input=new BufferedInputStream(new FileInputStream(file))) {
            byte[] buffer=new byte[65536];int count;
            while((count=input.read(buffer))!=-1)digest.update(buffer,0,count);
        }
        return ObjectPacks.hex(digest.digest());
    }

    private static void putStoredAligned(ZipOutputStream zip,String name,File file,int alignment) throws Exception {
        if(!(zip instanceof AlignedZipOutputStream))throw new IOException("ZIP stream cannot report offsets");
        long start=((AlignedZipOutputStream)zip).bytesWritten();
        int nameLength=name.getBytes(StandardCharsets.UTF_8).length;
        int padding=(int)(-(start+30+nameLength+4)&(alignment-1));
        byte[] extra=new byte[4+padding];put16(extra,0,0x4d54);put16(extra,2,padding);
        CRC32 crc=new CRC32();
        try(InputStream input=new BufferedInputStream(new FileInputStream(file))) {
            byte[] buffer=new byte[65536];int count;while((count=input.read(buffer))!=-1)crc.update(buffer,0,count);
        }
        ZipEntry entry=new ZipEntry(name);entry.setMethod(ZipEntry.STORED);
        entry.setSize(file.length());entry.setCompressedSize(file.length());entry.setCrc(crc.getValue());
        entry.setExtra(extra);zip.putNextEntry(entry);
        try(InputStream input=new BufferedInputStream(new FileInputStream(file))) {
            byte[] buffer=new byte[65536];int count;while((count=input.read(buffer))!=-1)zip.write(buffer,0,count);
        }
        zip.closeEntry();
        long payload=start+30+nameLength+extra.length;
        if(payload%alignment!=0)throw new IOException("EROFS ZIP payload is not aligned");
    }

    /** Identity of the launcher components: the label, MIME types and icon. */
    public static String launcherSplitName(String label,List<String> mimeTypes,byte[] iconPng) throws Exception {
        java.security.MessageDigest hash=java.security.MessageDigest.getInstance("SHA-256");
        update(hash,cleanLabel(label).getBytes(StandardCharsets.UTF_8));
        for(String mime:cleanMimeTypes(mimeTypes)) update(hash,mime.getBytes(StandardCharsets.UTF_8));
        update(hash,iconPng);
        return LAUNCHER_SPLIT_PREFIX+ObjectPacks.hex(hash.digest());
    }

    /** Identity of the extra output: index, metadata and, when it also carries
     * the launcher components, the label, MIME types and icon. */
    public static String extraSplitName(File objects) throws Exception {
        return extraSplitName(objects,null,null,null);
    }
    public static String extraSplitName(File objects,String launcherLabel,List<String> launcherMimeTypes,byte[] launcherIcon) throws Exception {
        java.security.MessageDigest hash=java.security.MessageDigest.getInstance("SHA-256");
        for(String name:Arrays.asList("objects.index","objects.ostree")) {
            byte[] bytes=java.nio.file.Files.readAllBytes(new File(objects,name).toPath());
            update(hash,bytes);
        }
        if(launcherLabel!=null&&launcherIcon!=null&&launcherIcon.length>0) {
            update(hash,cleanLabel(launcherLabel).getBytes(StandardCharsets.UTF_8));
            for(String mime:cleanMimeTypes(launcherMimeTypes)) update(hash,mime.getBytes(StandardCharsets.UTF_8));
            update(hash,launcherIcon);
        }
        return EXTRA_SPLIT_PREFIX+ObjectPacks.hex(hash.digest());
    }
    private static void update(java.security.MessageDigest hash,byte[] bytes) {
        hash.update(java.nio.ByteBuffer.allocate(8).putLong(bytes.length).array());hash.update(bytes);
    }

    /** Code-free launcher split: an {@code activity-alias} targeting the base
     * stub activity with the LAUNCHER filter and one VIEW filter per MIME type.
     * Its icon resource uses a distinct type id, so no ID can clash with the
     * base. The base stub keeps no launcher entry of its own. */
    public static File generateLauncher(File workDir,String pkg,String label,
            List<String> mimeTypes,byte[] iconPng,File output,int revision) throws Exception {
        if(revision<0)throw new IllegalArgumentException("Negative split revision");
        if(!packageName(pkg))throw new IllegalArgumentException("Invalid package name");
        if(iconPng==null||iconPng.length==0||iconPng.length>MAX_ICON_BYTES)throw new IllegalArgumentException("PNG icon size is invalid");
        String clean=cleanLabel(label);List<String> mimes=cleanMimeTypes(mimeTypes);
        String split=launcherSplitName(clean,mimes,iconPng);
        BinaryManifest manifest=new BinaryManifest(pkg,"","","flathub",Collections.emptyList());
        manifest.revisionCode=revision;
        File unsigned=File.createTempFile("matonos-launcher-",".apk",workDir);
        try {
            try(AlignedZipOutputStream zip=new AlignedZipOutputStream(new FileOutputStream(unsigned))) {
                put(zip,"AndroidManifest.xml",manifest.launcherManifest(split,clean,mimes,0x7f020000));
                put(zip,"resources.arsc",iconResourceTable(pkg,2));
                put(zip,"res/drawable/foreground.png",iconPng);
                put(zip,"res/drawable/icon.xml",new BinaryManifest("","","","flathub",Collections.emptyList()).adaptiveIcon(0x7f020000));
            }
            sign(unsigned,output);return output;
        } finally {unsigned.delete();}
    }

    /** Self-contained extra split: the applied extra output in the pack format,
     * with the index and metadata embedded so the loader can mount it directly.
     * When the launcher source is the extra output, the same split also carries
     * the launcher components and their resources. */
    public static File generateExtra(File workDir,String pkg,File objects,File output,int revision,
            String launcherLabel,List<String> launcherMimeTypes,byte[] launcherIcon) throws Exception {
        File unsigned=generateExtraUnsigned(workDir,pkg,objects,revision,launcherLabel,launcherMimeTypes,launcherIcon);
        try { sign(unsigned,output); return output; }
        finally { unsigned.delete(); }
    }

    /** Builds the package's extra split without signing it. */
    public static File generateExtraUnsigned(File workDir,String pkg,File objects,int revision,
            String launcherLabel,List<String> launcherMimeTypes,byte[] launcherIcon) throws Exception {
        if(revision<0)throw new IllegalArgumentException("Negative split revision");
        if(!packageName(pkg))throw new IllegalArgumentException("Invalid package name");
        if(!new File(objects,"objects.index").isFile()||!new File(objects,"objects.ostree").isFile())
            throw new IOException("Missing pack index or metadata");
        boolean launcher=launcherLabel!=null&&launcherIcon!=null&&launcherIcon.length>0;
        if(launcher&&launcherIcon.length>MAX_ICON_BYTES)throw new IllegalArgumentException("PNG icon size is invalid");
        List<ObjectPacks.Entry> entries=ObjectPacks.readIndex(new File(objects,"objects.index"));
        String pack=null;
        for(ObjectPacks.Entry entry:entries)if(entry.kind.equals("file")) {
            if(pack==null)pack=entry.pack;
            else if(!pack.equals(entry.pack))throw new IOException("Extra split requires a single pack");
        }
        if(pack==null)throw new IOException("Extra split has no payload pack");
        String split=extraSplitName(objects,launcher?launcherLabel:null,launcherMimeTypes,launcher?launcherIcon:null);
        BinaryManifest manifest=new BinaryManifest(pkg,"","","flathub",Collections.emptyList());
        manifest.revisionCode=revision;
        java.util.Map<String,byte[]> appended=new LinkedHashMap<>();
        if(launcher) appended.put("AndroidManifest.xml",manifest.launcherManifest(split,cleanLabel(launcherLabel),cleanMimeTypes(launcherMimeTypes),0x7f020000));
        else appended.put("AndroidManifest.xml",manifest.packManifest(split));
        appended.put("assets/matonos/objects.index",java.nio.file.Files.readAllBytes(new File(objects,"objects.index").toPath()));
        appended.put("assets/matonos/objects.ostree",java.nio.file.Files.readAllBytes(new File(objects,"objects.ostree").toPath()));
        if(launcher) {
            appended.put("resources.arsc",iconResourceTable(pkg,2));
            appended.put("res/drawable/foreground.png",launcherIcon);
            appended.put("res/drawable/icon.xml",new BinaryManifest("","","","flathub",Collections.emptyList()).adaptiveIcon(0x7f020000));
        }
        File unsigned=File.createTempFile("matonos-extra-",".apk",workDir);
        ObjectPacks.carrier(new File(objects,pack),unsigned,appended);
        return unsigned;
    }

    /** Validates an unsigned extra split's minimal signing identity. */
    /**
     * The extra split is built inside the package, next to vendor code, so the
     * subsystem only signs it when its manifest is exactly the identity plus a
     * code-free application and the APK carries no code.
     */
    public static boolean validExtraIdentity(File apk,String pkg,String split) throws Exception {
        if(!packageName(pkg)||split==null||!split.matches("extra_[0-9a-f]{64}")) return false;
        try(java.util.zip.ZipFile zip=new java.util.zip.ZipFile(apk)) {
            for(java.util.Enumeration<? extends java.util.zip.ZipEntry> e=zip.entries();e.hasMoreElements();) {
                String name=e.nextElement().getName();
                if(name.endsWith(".dex")||name.startsWith("lib/")||name.equals("resources.arsc"))return false;
            }
            java.util.zip.ZipEntry manifest=zip.getEntry("AndroidManifest.xml");
            if(manifest==null)return false;
            byte[] bytes;
            try(java.io.InputStream in=zip.getInputStream(manifest)){bytes=in.readAllBytes();}
            com.android.apksig.internal.apk.AndroidBinXmlParser parser =
                    new com.android.apksig.internal.apk.AndroidBinXmlParser(java.nio.ByteBuffer.wrap(bytes));
            boolean identity=false,codeFree=false;
            for(;parser.getEventType()!=com.android.apksig.internal.apk.AndroidBinXmlParser.EVENT_END_DOCUMENT;parser.next()) {
                if(parser.getEventType()!=com.android.apksig.internal.apk.AndroidBinXmlParser.EVENT_START_ELEMENT)continue;
                String element=parser.getName();
                if("manifest".equals(element)&&parser.getDepth()==1) {
                    String foundPackage=null,foundSplit=null;Integer version=null;
                    for(int i=0;i<parser.getAttributeCount();i++) {
                        String name=parser.getAttributeName(i);
                        if("package".equals(name))foundPackage=parser.getAttributeStringValue(i);
                        else if("split".equals(name))foundSplit=parser.getAttributeStringValue(i);
                        else if("versionCode".equals(name))version=parser.getAttributeIntValue(i);
                    }
                    identity=pkg.equals(foundPackage)&&split.equals(foundSplit)&&version!=null&&version==STUB_VERSION_CODE;
                } else if("application".equals(element)&&parser.getDepth()==2) {
                    for(int i=0;i<parser.getAttributeCount();i++)
                        if("hasCode".equals(parser.getAttributeName(i)))codeFree=!parser.getAttributeBooleanValue(i);
                } else if(!("uses-sdk".equals(element)&&parser.getDepth()==2)) return false;
            }
            return identity&&codeFree;
        }
    }

    /** Signs an identity-checked package-built extra split with the subsystem key. */
    public static File signExtra(File unsigned,File output) throws Exception {
        sign(unsigned,output); return output;
    }

    private static boolean packageName(String pkg) {
        return pkg!=null&&pkg.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+");
    }

    /** Legacy compatibility: no commit info, no images. */
    public static File generate(File workDir, String ref, String label, byte[] iconPng,
            List<String> permissions, String packageName, File output) throws Exception {
        return generate(workDir, ref, label, iconPng, permissions, packageName, output, null, null);
    }

    /** The launcher label: the caller's name, or the generator's last resort. */
    static String cleanLabel(String label) {
        if (label == null) return "Linux application";
        String name = label.trim();
        if (name.isEmpty()) return "Linux application";
        if (name.getBytes(StandardCharsets.UTF_8).length > 4096) return "Linux application";
        return name;
    }

    /** Parsed Flatpak {@code .desktop} launcher data. */
    public static final class DesktopEntry {
        public final String name;
        public final String iconName;
        public final List<String> mimeTypes;
        DesktopEntry(String name, String iconName, List<String> mimeTypes) {
            this.name = name; this.iconName = iconName; this.mimeTypes = mimeTypes;
        }
    }

    /** Reads the {@code [Desktop Entry]} group's name, icon name and MIME types
     * only. Localized keys ({@code Name[de]}) are ignored; comments do not
     * count. */
    public static DesktopEntry parseDesktop(String content) throws IOException {
        if (content == null) throw new IOException("No desktop entry content");
        String name = null, icon = null;
        List<String> mimes = new ArrayList<>();
        boolean inEntry = false, seen = false;
        for (String raw : content.split("\n", -1)) {
            String line = raw.endsWith("\r") ? raw.substring(0, raw.length() - 1) : raw;
            String trimmed = line.trim();
            if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
                if (inEntry) break;
                inEntry = "[Desktop Entry]".equals(trimmed);
                if (inEntry) seen = true;
                continue;
            }
            if (!inEntry || trimmed.isEmpty() || trimmed.startsWith("#")) continue;
            int equals = line.indexOf('=');
            if (equals < 0) continue;
            String key = line.substring(0, equals).trim();
            String value = line.substring(equals + 1).trim();
            if (key.equals("Name")) name = value;
            else if (key.equals("Icon") && validIconName(value)) icon = value;
            else if (key.equals("MimeType"))
                for (String item : value.split(";")) {
                    String mime = item.trim();
                    if (mime.length() <= 255
                            && mime.matches("[A-Za-z0-9!#$&^_.+-]+/[A-Za-z0-9!#$&^_.+-]+")
                            && !mimes.contains(mime)) mimes.add(mime);
                }
        }
        if (!seen) throw new IOException(".desktop file has no [Desktop Entry] group");
        return new DesktopEntry(cleanLabel(name), icon, cleanMimeTypes(mimes));
    }

    /** A theme icon name: a plain file name, never a path or an absolute name. */
    private static boolean validIconName(String value) {
        return !value.isEmpty() && value.length() <= 255
                && value.matches("[A-Za-z0-9_.+-]+")
                && !value.equals(".") && !value.equals("..");
    }


    /** Bounded, de-duplicated, syntactically valid MIME type list. */
    public static List<String> cleanMimeTypes(List<String> mimeTypes) {
        List<String> clean = new ArrayList<>();
        if (mimeTypes == null) return clean;
        for (String item : mimeTypes) {
            if (item == null) continue;
            String mime = item.trim();
            if (mime.length() <= 255
                    && mime.matches("[A-Za-z0-9!#$&^_.+-]+/[A-Za-z0-9!#$&^_.+-]+")
                    && !clean.contains(mime)) clean.add(mime);
            if (clean.size() >= 128) break;
        }
        return clean;
    }

    private static boolean validRef(String ref) {
        if (ref == null || ref.length() < 12 || ref.length() > 512) return false;
        String[] parts = ref.split("/", -1);
        if (parts.length != 4 || !(parts[0].equals("app") || parts[0].equals("runtime")) ||
                !validAppId(parts[1]) || !validRefPart(parts[2]) || !validRefPart(parts[3])) return false;
        for (String part : parts) if (part.equals(".") || part.equals("..")) return false;
        return true;
    }

    private static boolean validRefPart(String value) {
        return value.length() > 0 && value.length() <= 96 && value.matches("[A-Za-z0-9_.-]+") &&
                !value.equals(".") && !value.equals("..");
    }

    private static boolean validAppId(String value) {
        if (value.length() < 3 || value.length() > 255 || value.startsWith(".") || value.endsWith(".")) return false;
        int lastDot = value.lastIndexOf('.');
        if (lastDot < 0) return false;
        int component = 0;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c == '.') {
                if (component == 0 || component > 63 || value.charAt(i - 1) == '-') return false;
                component = 0;
            } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_') {
                if (component == 0 && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
                component++;
            } else if (c == '-' && i > lastDot && component > 0 && i + 1 < value.length() && value.charAt(i + 1) != '.') {
                component++;
            } else return false;
        }
        return component > 0 && component <= 63;
    }

    private static void writeUnsigned(File file, String pkg, String label, String ref, String remote,
            byte[] icon, List<String> permissions, CommitInfo commitInfo, int minInterfaceVersion,
            File packDirectory) throws IOException {
        writeUnsigned(file,pkg,label,ref,remote,icon,permissions,commitInfo,minInterfaceVersion,packDirectory,null);
    }
    private static void writeUnsigned(File file, String pkg, String label, String ref, String remote,
            byte[] icon, List<String> permissions, CommitInfo commitInfo, int minInterfaceVersion,
            File packDirectory, String packs) throws IOException {
        writeUnsigned(file,pkg,label,ref,remote,icon,permissions,commitInfo,minInterfaceVersion,packDirectory,packs,null);
    }
    private static void writeUnsigned(File file,String pkg,String label,String ref,String remote,byte[] icon,List<String> permissions,CommitInfo commitInfo,int minInterfaceVersion,File packDirectory,String packs,String dependencyPackages) throws IOException {
        List<String> cleanPermissions = new ArrayList<>();
        if (permissions != null) for (String p : permissions) {
            if (cleanPermissions.size() >= 128 || (p != null && p.length() > 255)) throw new IllegalArgumentException("Too many or oversized Android permissions");
            if (p == null || !p.matches("[A-Za-z0-9_.]+")) throw new IllegalArgumentException("Invalid Android permission: " + p);
            if (!cleanPermissions.contains(p)) cleanPermissions.add(p);
        }
        // Validate commit info if provided (for new-format stubs with commit metadata)
        // null commitInfo is allowed for backward-compatible no-image stubs.
        if (commitInfo != null) {
            if (!validCommit(commitInfo.appCommit))
                throw new IllegalArgumentException("Invalid app commit checksum");
            if (!validRef(commitInfo.runtimeRef) || !commitInfo.runtimeRef.startsWith("runtime/"))
                throw new IllegalArgumentException("Invalid runtime ref in commit info");
            if (!validCommit(commitInfo.runtimeCommit))
                throw new IllegalArgumentException("Invalid runtime commit checksum");
        }
        BinaryManifest encoder = new BinaryManifest(pkg, label, ref, remote, cleanPermissions);
        encoder.commitInfo = commitInfo;
        encoder.minInterfaceVersion = minInterfaceVersion;
        encoder.packs = packs;
        encoder.dependencyPackages=dependencyPackages;
        byte[] manifest = encoder.encode();
        try (AlignedZipOutputStream zip = new AlignedZipOutputStream(new FileOutputStream(file))) {
            put(zip, "AndroidManifest.xml", manifest);
            // Supply a real drawable resource so Android launchers can resolve the app icon.
            put(zip, "resources.arsc", iconResourceTable(pkg));
            put(zip, "classes.dex", loaderDex());
            put(zip, "res/drawable/foreground.png", icon);
            put(zip, "res/drawable/icon.xml", new BinaryManifest("", "", "", "flathub", Collections.emptyList()).adaptiveIcon());
            File layersIndex=packDirectory==null?null:new File(packDirectory,"layers.index");
            if(layersIndex!=null&&layersIndex.isFile()) {
                byte[] index=java.nio.file.Files.readAllBytes(layersIndex.toPath());
                if(index.length<12||!Arrays.equals(Arrays.copyOf(index,8),"MTPKIDX2".getBytes(StandardCharsets.US_ASCII)))
                    throw new IOException("Invalid MTPKIDX2 layers.index");
                put(zip,"assets/matonos/layers.index",index);
            }
        }
    }


    private static byte[] iconResourceTable(String pkg) throws IOException {
        return iconResourceTable(pkg,1);
    }

    /** Adaptive icon XML, foreground bitmap and background color resources.
     * The background must be a reference: density-specific drawable inflation
     * resolves a layer's resource ID, which is zero for an inline XML color.
     *
     * <p>A split resource table uses a distinct type id (2) so its entries can
     * never share a resource ID with the base's type id 1 entries. The type
     * string pool is offset with a placeholder so the split's "drawable" type
     * keeps that id.
     */
    private static byte[] iconResourceTable(String pkg,int typeId) throws IOException {
        byte[] global = resourceStringPool("res/drawable/icon.xml", "res/drawable/foreground.png");
        byte[] types = typeId==1?resourceStringPool("drawable"):resourceStringPool("matonos","drawable");
        byte[] keys = resourceStringPool("icon", "foreground", "background");
        byte[] spec = new byte[28];
        put16(spec,0,0x0202); put16(spec,2,16); putInt(spec,4,spec.length);
        spec[8]=(byte)typeId; putInt(spec,12,3);
        byte[] type = new byte[144];
        put16(type,0,0x0201); put16(type,2,84); putInt(type,4,type.length);
        type[8]=(byte)typeId; putInt(type,12,3); putInt(type,16,96); putInt(type,20,64);
        putInt(type,84,0); putInt(type,88,16); putInt(type,92,32);
        for (int i=0;i<3;i++) {
            int offset=96+i*16; put16(type,offset,8); putInt(type,offset+4,i);
            put16(type,offset+8,8); type[offset+11]=(byte)(i==2?0x1c:3); putInt(type,offset+12,i==2?0xfff5f5f5:i);
        }
        byte[] header = new byte[288];
        put16(header,0,0x0200); put16(header,2,288);
        putInt(header,4,288+types.length+keys.length+spec.length+type.length); putInt(header,8,0x7f);
        byte[] name = pkg.getBytes(StandardCharsets.UTF_16LE);
        System.arraycopy(name,0,header,12,Math.min(name.length,254));
        putInt(header,268,288); putInt(header,272,typeId);
        putInt(header,276,288+types.length); putInt(header,280,3);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        BinaryManifest.write16(out,2); BinaryManifest.write16(out,12);
        BinaryManifest.write32(out,12+global.length+header.length+types.length+keys.length+spec.length+type.length);
        BinaryManifest.write32(out,1);
        out.write(global); out.write(header); out.write(types); out.write(keys); out.write(spec); out.write(type);
        return out.toByteArray();
    }

    private static byte[] resourceStringPool(String... values) throws IOException {
        BinaryManifest encoder = new BinaryManifest("","","","flathub",Collections.emptyList());
        for (String value: values) encoder.str(value);
        ByteArrayOutputStream out = new ByteArrayOutputStream(); encoder.writeStringPool(out); return out.toByteArray();
    }

    /**
     * The stub's own code: StubLoader, which finds the installed subsystem APK
     * through PackageManager on every launch. Stubs declare no uses-library,
     * so no library path is recorded for them that a host update could stale.
     */
    private static byte[] loaderDex() throws IOException {
        try (java.io.InputStream in = StubGenerator.class.getResourceAsStream("stub-loader.dex")) {
            if (in == null) throw new IOException("Missing bundled stub-loader.dex");
            return in.readAllBytes();
        }
    }

    private static void putInt(byte[] out, int offset, int value) {
        out[offset] = (byte) value; out[offset + 1] = (byte) (value >>> 8);
        out[offset + 2] = (byte) (value >>> 16); out[offset + 3] = (byte) (value >>> 24);
    }
    private static void put16(byte[] out, int offset, int value) {
        out[offset] = (byte) value; out[offset + 1] = (byte) (value >>> 8);
    }
    private static void put32(byte[] out, int offset, int value) { putInt(out, offset, value); }

    private static void put(ZipOutputStream zip, String name, byte[] bytes) throws IOException {
        ZipEntry entry = new ZipEntry(name);
        // Android's package parser reads binary XML directly from the APK; keep it uncompressed.
        // Store all v1 contents for a simpler, deterministic ZIP layout.
        if ("resources.arsc".equals(name) && zip instanceof AlignedZipOutputStream) {
            byte[] extra = alignmentExtra(((AlignedZipOutputStream) zip).bytesWritten(), name);
            if (extra.length != 0) entry.setExtra(extra);
        }
        CRC32 crc = new CRC32(); crc.update(bytes);
        entry.setMethod(ZipEntry.STORED); entry.setSize(bytes.length);
        entry.setCompressedSize(bytes.length); entry.setCrc(crc.getValue());
        zip.putNextEntry(entry); zip.write(bytes); zip.closeEntry();
    }

    /** Pad stored resource tables to the four-byte offset required by PackageManager. */
    static byte[] alignmentExtra(long localHeaderOffset, String name) {
        int nameLength = name.getBytes(StandardCharsets.UTF_8).length;
        int padding = (int) (-(localHeaderOffset + 30 + nameLength + 4) & 3);
        if (padding == 0) return new byte[0];
        byte[] extra = new byte[4 + padding];
        put16(extra, 0, 0xd935); // private ZIP extra field used only for alignment padding
        put16(extra, 2, padding);
        return extra;
    }

    private static final class AlignedZipOutputStream extends ZipOutputStream {
        private final CountingOutputStream counter;
        AlignedZipOutputStream(OutputStream output) {
            this(new CountingOutputStream(output));
        }
        private AlignedZipOutputStream(CountingOutputStream output) {
            super(output); counter = output;
        }
        long bytesWritten() { return counter.count; }
    }

    private static final class CountingOutputStream extends FilterOutputStream {
        long count;
        CountingOutputStream(OutputStream output) { super(output); }
        @Override public void write(int value) throws IOException { out.write(value); count++; }
        @Override public void write(byte[] bytes, int offset, int length) throws IOException {
            out.write(bytes, offset, length); count += length;
        }
    }

    /** Read-only attestation: do not mint a replacement key during verification. */
    public static byte[] getExistingSigningCertificate() throws Exception {
        KeyStore store=KeyStore.getInstance("AndroidKeyStore");store.load(null);
        java.security.cert.Certificate certificate=store.getCertificate(KEY_ALIAS);
        return certificate!=null?certificate.getEncoded():null;
    }

    private static synchronized KeyMaterial getOrCreateKey() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore"); store.load(null);
        if (!store.containsAlias(KEY_ALIAS)) {
            KeyPairGenerator generator = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore");
            generator.initialize(new KeyGenParameterSpec.Builder(KEY_ALIAS,
                    KeyProperties.PURPOSE_SIGN | KeyProperties.PURPOSE_VERIFY)
                    .setAlgorithmParameterSpec(new java.security.spec.ECGenParameterSpec("secp256r1"))
                    .setDigests(KeyProperties.DIGEST_SHA256).build());
            generator.generateKeyPair();
            store.load(null);
        }
        PrivateKey key = (PrivateKey) store.getKey(KEY_ALIAS, null);
        X509Certificate certificate = (X509Certificate) store.getCertificate(KEY_ALIAS);
        if (key == null || certificate == null) throw new IllegalStateException("Stub signing key unavailable in Android Keystore");
        return new KeyMaterial(key, certificate);
    }

    private static void sign(File input, File output) throws Exception {
        if (testSigningBackend != null) { testSigningBackend.sign(input, output); return; }
        sign(input, output, getOrCreateKey());
    }
    private static void sign(File input, File output, KeyMaterial material) throws Exception {
        SigningBackend testBackend = testSigningBackend;
        if (testBackend != null) { testBackend.sign(input, output); return; }
        ApkSigner.SignerConfig signer = new ApkSigner.SignerConfig.Builder("matonos-stub", material.key,
                Collections.singletonList(material.certificate)).build();
        new ApkSigner.Builder(Collections.singletonList(signer)).setInputApk(input).setOutputApk(output)
                // Stubs require API 30; v2/v3 cover their supported Android versions.
                .setV1SigningEnabled(false).setV2SigningEnabled(true).setV3SigningEnabled(true)
                .setV4SigningEnabled(false).setAlignmentPreserved(true).build().sign();
    }

    private static final class KeyMaterial {
        final PrivateKey key; final X509Certificate certificate;
        KeyMaterial(PrivateKey key, X509Certificate certificate) { this.key = key; this.certificate = certificate; }
    }

    /** Minimal Android binary XML encoder for the manifest vocabulary used by stubs. */
    private static final class BinaryManifest {
        private static final String ANDROID = "http://schemas.android.com/apk/res/android";
        private static final int ANDROID_URI = 0x01000000;
        private final String pkg, label, ref, remote;
        private final List<String> permissions;
        private final LinkedHashMap<String, Integer> strings = new LinkedHashMap<>();
        private final List<Chunk> nodes = new ArrayList<>();
        BinaryManifest(String pkg, String label, String ref, String remote, List<String> permissions) {
            this.pkg = pkg; this.label = label; this.ref = ref; this.remote = remote; this.permissions = permissions;
        }
        CommitInfo commitInfo;
        String packs;
        String dependencyPackages;
        boolean noActivities;
        int minInterfaceVersion = 2;
        int revisionCode;
        byte[] encode() throws IOException {
            for (String s : Arrays.asList(ANDROID, "android", "manifest", "package", "versionCode", "versionName", "uses-sdk", "minSdkVersion", "targetSdkVersion", "uses-permission", "name", "application", "label", "hasCode", "appComponentFactory", "activity", "exported", "enabled", "meta-data", "value", "intent-filter", "action", "category", "service", "org.matonos.linux.stub.StubActivity", "org.matonos.linux.stub.StubService", HOST_LOADER, INSTALL_ACTIVITY, INSTALL_ACTION, UPDATE_ACTIVITY, UPDATE_ACTION, "android.intent.action.MAIN", "android.intent.category.DEFAULT", REF_META, REMOTE_META, MIN_INTERFACE_META, APP_COMMIT_META, RUNTIME_REF_META, RUNTIME_COMMIT_META, PACKS_META, DEPENDENCIES_META, APP_REF_META, EXTRA_SERVICE, EXTRA_MOUNT_META, APP_MOUNT, "targetActivity", "activity-alias", "data", "android.intent.category.LAUNCHER", "android.intent.action.VIEW", pkg, label, ref, remote, "1", "1.0", "30", "36")) str(s);
            for (String p : permissions) str(p);
            // Pre-intern commit info values if present
            if (commitInfo != null) {
                str(commitInfo.appCommit);
                str(commitInfo.runtimeRef);
                str(commitInfo.runtimeCommit);
            }
            if (packs != null) str(packs);
            if (dependencyPackages != null) str(dependencyPackages);
            str("intent");str("org.matonos.linuxhost.PACKAGE");
            namespace(true);
            List<Attr> root = new ArrayList<>(attrs(a("package", pkg), ai("versionCode", STUB_VERSION_CODE), a("versionName", "1.0")));
            start("manifest", null, root);
            start("uses-sdk", null, attrs(ai("minSdkVersion", 30), ai("targetSdkVersion", 36)));
            end("uses-sdk");
            startEnd("uses-permission",attrs(a("name","android.permission.FOREGROUND_SERVICE")));
            startEnd("uses-permission",attrs(a("name","android.permission.FOREGROUND_SERVICE_DATA_SYNC")));
            startEnd("uses-permission",attrs(a("name","android.permission.FOREGROUND_SERVICE_SPECIAL_USE")));
            // The stub holds portal Inhibit and install/update wake locks in its own UID.
            startEnd("uses-permission",attrs(a("name","android.permission.WAKE_LOCK")));
            // "install me" downloads as the stub's UID (netd DNS + sockets need it).
            startEnd("uses-permission",attrs(a("name","android.permission.INTERNET")));
            start("queries",null,attrs());
            startEnd("package",attrs(a("name","org.matonos.linux")));
            start("intent",null,attrs());
            startEnd("action",attrs(a("name","org.matonos.linuxhost.PACKAGE")));
            end("intent");
            end("queries");
            for (String p : permissions) startEnd("uses-permission", attrs(a("name", p)));
            start("application", null, attrs(a("label", label), new Attr("icon", "@drawable/icon", 0x7f010000, 1), ab("hasCode", true), a("appComponentFactory", HOST_LOADER), new Attr("theme","@android:style/Theme.Material.Light.NoActionBar",android.R.style.Theme_Material_Light_NoActionBar,1)));
            startEnd("meta-data",attrs(a("name",REF_META),a("value",ref)));
            startEnd("meta-data",attrs(a("name",APP_REF_META),a("value",ref)));
            if(dependencyPackages!=null&&!dependencyPackages.isEmpty())startEnd("meta-data",attrs(a("name",DEPENDENCIES_META),a("value",dependencyPackages)));
            if(noActivities){
                start("service", null, attrs(a("name", EXTRA_SERVICE), ab("exported", true), ab("enabled", true)));
                startEnd("meta-data", attrs(a("name", REF_META), a("value", ref)));
                startEnd("meta-data", attrs(a("name", EXTRA_MOUNT_META), a("value", packs)));
                start("intent-filter",null,attrs());startEnd("action",attrs(a("name","org.matonos.linuxhost.PACKAGE")));end("intent-filter");
                end("service");
                startEnd("meta-data",attrs(a("name",PACKS_META),a("value",packs)));
                end("application");end("manifest");namespace(false);return finish();
            }
            startEnd("provider", attrs(a("name", "org.matonos.linux.PortalFiles"),
                    a("authorities", pkg + ".portal.files"), ab("exported", false), ab("grantUriPermissions", true)));
            start("activity", null, attrs(a("name", HOST_ACTIVITY), ab("exported", true), ab("enabled", true), a("label", label), new Attr("icon", "@drawable/icon", 0x7f010000, 1)));
            startEnd("meta-data", attrs(a("name", REF_META), a("value", ref)));
            startEnd("meta-data", attrs(a("name", REMOTE_META), a("value", remote)));
            startEnd("meta-data", attrs(a("name", MIN_INTERFACE_META), ai("value", minInterfaceVersion)));
            if (commitInfo != null) {
                startEnd("meta-data", attrs(a("name", APP_COMMIT_META), a("value", commitInfo.appCommit)));
                startEnd("meta-data", attrs(a("name", RUNTIME_REF_META), a("value", commitInfo.runtimeRef)));
                startEnd("meta-data", attrs(a("name", RUNTIME_COMMIT_META), a("value", commitInfo.runtimeCommit)));
            }
            if (packs != null) startEnd("meta-data", attrs(a("name", PACKS_META), a("value", packs)));
            if (dependencyPackages != null && !dependencyPackages.isEmpty()) startEnd("meta-data",attrs(a("name",DEPENDENCIES_META),a("value",dependencyPackages)));
            startEnd("meta-data",attrs(a("name",APP_REF_META),a("value",ref)));
            // The base stub deliberately has no MAIN/LAUNCHER filter: the
            // launcher entry lives in the code-free launcher split so a label
            // or icon change never updates the base.
            end("activity");
            // Stays enabled while the launcher is hidden, so the store can
            // resume an install interrupted by a crash or power cut.
            start("activity", null, attrs(a("name", INSTALL_ACTIVITY), ab("exported", true),
                    new Attr("theme", "@android:style/Theme.Translucent.NoTitleBar", android.R.style.Theme_Translucent_NoTitleBar, 1)));
            startEnd("meta-data", attrs(a("name", REF_META), a("value", ref)));
            start("intent-filter", null, attrs());
            startEnd("action", attrs(a("name", INSTALL_ACTION)));
            startEnd("category", attrs(a("name", "android.intent.category.DEFAULT")));
            end("intent-filter");
            end("activity");
            start("activity", null, attrs(a("name", UPDATE_ACTIVITY), ab("exported", true),
                    new Attr("theme", "@android:style/Theme.Translucent.NoTitleBar", android.R.style.Theme_Translucent_NoTitleBar, 1)));
            startEnd("meta-data", attrs(a("name", REF_META), a("value", ref)));
            start("intent-filter", null, attrs());
            startEnd("action", attrs(a("name", UPDATE_ACTION)));
            startEnd("category", attrs(a("name", "android.intent.category.DEFAULT")));
            end("intent-filter");
            end("activity");
            start("service", null, attrs(a("name", HOST_SERVICE), ab("exported", false), ai("foregroundServiceType", android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)));
            startEnd("property", attrs(a("name", "android.app.PROPERTY_SPECIAL_USE_FGS_SUBTYPE"),
                    a("value", "Owns the Linux application runtime and its background network operations")));
            startEnd("meta-data", attrs(a("name", REF_META), a("value", ref)));
            end("service");
            // Extra data is arbitrary vendor code: the stub's own UID runs
            // apply_extra, never the installer. Exported only so the subsystem
            // app can bind it; the service refuses every other caller's UID.
            start("service", null, attrs(a("name", EXTRA_SERVICE), ab("exported", true), ab("enabled", true)));
            startEnd("meta-data", attrs(a("name", REF_META), a("value", ref)));
            startEnd("meta-data", attrs(a("name", EXTRA_MOUNT_META), a("value", APP_MOUNT)));
            start("intent-filter",null,attrs());startEnd("action",attrs(a("name","org.matonos.linuxhost.PACKAGE")));end("intent-filter");
            end("service");
            startEnd("service", attrs(a("name", "org.matonos.linux.stub.InstallService"),
                    ab("exported", false), ai("foregroundServiceType", 1)));
            end("application"); end("manifest"); namespace(false);
            return finish();
        }
        byte[] packManifest(String split) throws IOException {
            namespace(true);
            start("manifest",null,attrs(a("package",pkg),a("split",split),ai("versionCode",STUB_VERSION_CODE),ai("revisionCode",revisionCode)));
            startEnd("uses-sdk",attrs(ai("minSdkVersion",30),ai("targetSdkVersion",36)));
            startEnd("application",attrs(ab("hasCode",false)));
            end("manifest");namespace(false);return finish();
        }
        /** Launcher split manifest: an activity-alias that targets the base
         * stub activity, with the LAUNCHER filter and VIEW filters. */
        byte[] launcherManifest(String split,String launcherLabel,List<String> mimeTypes,int iconId) throws IOException {
            for (String s : Arrays.asList("targetActivity","activity-alias","data","android.intent.action.VIEW",
                    "android.intent.action.MAIN","android.intent.category.LAUNCHER","android.intent.category.DEFAULT",
                    HOST_ACTIVITY,pkg,launcherLabel)) str(s);
            for (String mime:mimeTypes) str(mime);
            namespace(true);
            start("manifest",null,attrs(a("package",pkg),a("split",split),ai("versionCode",STUB_VERSION_CODE),ai("revisionCode",revisionCode)));
            startEnd("uses-sdk",attrs(ai("minSdkVersion",30),ai("targetSdkVersion",36)));
            start("application",null,attrs(ab("hasCode",false)));
            start("activity-alias",null,attrs(a("name",pkg+".Launcher"),a("targetActivity",HOST_ACTIVITY),
                    ab("exported",true),ab("enabled",true),a("label",launcherLabel),
                    new Attr("icon","@drawable/icon",iconId,1)));
            start("intent-filter",null,attrs());
            startEnd("action",attrs(a("name","android.intent.action.MAIN")));
            startEnd("category",attrs(a("name","android.intent.category.LAUNCHER")));
            end("intent-filter");
            for(String mime:mimeTypes) {
                start("intent-filter",null,attrs());
                startEnd("action",attrs(a("name","android.intent.action.VIEW")));
                startEnd("category",attrs(a("name","android.intent.category.DEFAULT")));
                startEnd("data",attrs(a("mimeType",mime)));
                end("intent-filter");
            }
            end("activity-alias");
            end("application");end("manifest");namespace(false);return finish();
        }
        private byte[] finish() throws IOException {
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            writeStringPool(body); writeResourceMap(body);
            for (Chunk chunk : nodes) chunk.write(body);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            write16(out, 0x0003); write16(out, 8); write32(out, 8 + body.size()); body.writeTo(out); return out.toByteArray();
        }
        byte[] adaptiveIcon() throws IOException { return adaptiveIcon(0x7f010000); }
        byte[] adaptiveIcon(int base) throws IOException {
            namespace(true);
            start("adaptive-icon", null, attrs());
            startEnd("background", attrs(new Attr("drawable", "@drawable/background", base+2, 1)));
            startEnd("foreground", attrs(new Attr("drawable", "@drawable/foreground", base+1, 1)));
            end("adaptive-icon"); namespace(false);
            ByteArrayOutputStream body=new ByteArrayOutputStream();
            writeStringPool(body);writeResourceMap(body);
            for (Chunk chunk:nodes) chunk.write(body);
            ByteArrayOutputStream out=new ByteArrayOutputStream();
            write16(out,3);write16(out,8);write32(out,8+body.size());body.writeTo(out);
            return out.toByteArray();
        }
        private int str(String s) { Integer i = strings.get(s); if (i != null) return i; int n = strings.size(); strings.put(s, n); return n; }
        private Attr a(String name, String value) { return new Attr(name, value, 0, 3); }
        private Attr ai(String name, int value) { return new Attr(name, Integer.toString(value), value, 0x10); }
        private Attr ab(String name, boolean value) { return new Attr(name, value ? "true" : "false", value ? 1 : 0, 0x12); }
        private List<Attr> attrs(Attr... a) { return Arrays.asList(a); }
        private void namespace(boolean start) throws IOException { int t = start ? 0x0100 : 0x0101; nodes.add(Chunk.namespace(t, str("android"), str(ANDROID))); }
        private void start(String name, String text, List<Attr> attrs) throws IOException {
            // Android's TypedArray retrieval walks attributes by resource ID.
            // Unsorted attributes can silently disappear during manifest parsing.
            attrs = new ArrayList<>(attrs);
            attrs.sort((left, right) -> Integer.compareUnsigned(resourceId(left.name), resourceId(right.name)));
            for (Attr attr : attrs) { str(attr.name); str(attr.value); }
            nodes.add(Chunk.start(str(name), attrs, strings, str(ANDROID)));
        }
        private void startEnd(String name, List<Attr> attrs) throws IOException { start(name, null, attrs); end(name); }
        private void end(String name) throws IOException { nodes.add(Chunk.end(str(name))); }
        private void writeStringPool(ByteArrayOutputStream out) throws IOException {
            ByteArrayOutputStream data = new ByteArrayOutputStream(); List<Integer> offsets = new ArrayList<>();
            for (String s : strings.keySet()) {
                offsets.add(data.size()); byte[] utf8 = s.getBytes(StandardCharsets.UTF_8); write8Length(data, s.length()); write8Length(data, utf8.length); data.write(utf8); data.write(0);
            }
            while ((data.size() & 3) != 0) data.write(0);
            int header = 28, start = header + offsets.size() * 4, size = start + data.size();
            write16(out, 1); write16(out, header); write32(out, size); write32(out, offsets.size()); write32(out, 0); write32(out, 0x100); write32(out, start); write32(out, 0);
            for (int offset : offsets) write32(out, offset); data.writeTo(out);
        }
        private void writeResourceMap(ByteArrayOutputStream out) throws IOException {
            int[] ids = new int[strings.size()];
            for (String name : Arrays.asList("authorities", "grantUriPermissions", "version", "certDigest", "theme", "foregroundServiceType", "appComponentFactory", "drawable", "name", "label", "icon", "exported", "enabled", "hasCode", "required", "value", "mimeType", "targetActivity", "versionCode", "revisionCode", "versionName", "minSdkVersion", "targetSdkVersion")) {
                Integer i = strings.get(name); if (i != null) ids[i] = resourceId(name);
            }
            write16(out, 0x0180); write16(out, 8); write32(out, 8 + ids.length * 4); for (int id : ids) write32(out, id);
        }
        private int resourceId(String name) {
            switch (name) {
                case "authorities": return android.R.attr.authorities;
                case "grantUriPermissions": return android.R.attr.grantUriPermissions;
                case "version": return 0x01010519;
                case "certDigest": return 0x01010548;
                case "theme": return 0x01010000;
                case "foregroundServiceType": return android.R.attr.foregroundServiceType;
                case "appComponentFactory": return android.R.attr.appComponentFactory;
                case "drawable": return 0x01010199; case "icon": return 0x01010002; case "name": return 0x01010003; case "label": return 0x01010001; case "exported": return 0x01010010; case "enabled": return 0x0101000e;
                case "hasCode": return 0x0101000c; case "required": return 0x0101028e; case "value": return 0x01010024;
                case "revisionCode": return 0x010104d5;
                case "mimeType": return 0x01010026; case "versionCode": return 0x0101021b; case "versionName": return 0x0101021c;
                case "targetActivity": return 0x01010202;
                case "minSdkVersion": return 0x0101020c; case "targetSdkVersion": return 0x01010270; default: return 0;
            }
        }
        private static void write8Length(ByteArrayOutputStream out, int n) {
            if (n < 0 || n > 32767) throw new IllegalArgumentException("Manifest string is too large");
            if (n > 127) { out.write((n >> 8) | 0x80); out.write(n & 0xff); } else out.write(n);
        }
        private static void write16(ByteArrayOutputStream out, int n) { out.write(n & 255); out.write((n >> 8) & 255); }
        private static void write32(ByteArrayOutputStream out, int n) { write16(out, n); write16(out, n >>> 16); }
        private static final class Attr {
            final String name, value; final int data, type;
            Attr(String name, String value, int data, int type) { this.name = name; this.value = value; this.data = data; this.type = type; }
        }
        private static final class Chunk {
            final byte[] bytes; Chunk(byte[] bytes) { this.bytes = bytes; }
            void write(ByteArrayOutputStream out) throws IOException { out.write(bytes); }
            static Chunk namespace(int type, int prefix, int uri) throws IOException {
                ByteArrayOutputStream out = node(type, 24); write32(out, prefix); write32(out, uri); return new Chunk(out.toByteArray());
            }
            static Chunk start(int name, List<Attr> attrs, Map<String, Integer> strings, int androidUri) throws IOException {
                ByteArrayOutputStream out = node(0x0102, 36 + attrs.size() * 20);
                write32(out, -1); write32(out, name);
                write16(out, 20); write16(out, 20); write16(out, attrs.size()); write16(out, 0); write16(out, 0); write16(out, 0);
                for (Attr a : attrs) {
                    Integer nameIndex = strings.get(a.name);
                    Integer valueIndex = strings.get(a.value);
                    if (nameIndex == null || valueIndex == null) throw new IllegalStateException("Uninterned manifest attribute " + a.name + "=" + a.value);
                    write32(out, ("package".equals(a.name) || "split".equals(a.name)) ? -1 : androidUri); write32(out, nameIndex); write32(out, valueIndex);
                    write16(out, 8); out.write(0); out.write(a.type); write32(out, a.type == 3 ? strings.get(a.value) : a.data);
                }
                return new Chunk(out.toByteArray());
            }
            static Chunk end(int name) throws IOException { ByteArrayOutputStream out = node(0x0103, 24); write32(out, -1); write32(out, name); return new Chunk(out.toByteArray()); }
            private static ByteArrayOutputStream node(int type, int size) throws IOException { ByteArrayOutputStream out = new ByteArrayOutputStream(); write16(out, type); write16(out, 16); write32(out, size); write32(out, 1); write32(out, -1); return out; }
        }
    }
}
