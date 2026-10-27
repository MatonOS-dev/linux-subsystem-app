package org.matonos.linuxhost.stubgen;

import java.io.File;
import java.io.RandomAccessFile;
import java.nio.file.Files;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import org.matonos.linuxhost.stubgen.PlanExecutor;
import org.json.JSONArray;
import org.json.JSONObject;
import org.matonos.linux.stub.DeployManifest;
import org.matonos.linux.stub.InstallHandoff;
import org.matonos.linux.stub.DeployRecord;
import org.matonos.linuxhost.stubgen.DependencyPlan;
import java.nio.charset.StandardCharsets;

/** Host regression for the untyped transaction-row to package-manifest boundary. */
public final class InstallHandoffTest {
    private static final String APP="app/org.example.Editor/x86_64/stable";
    private static final String RUNTIME="runtime/org.example.Platform/x86_64/25.08";
    private static final String LOCALE="runtime/org.freedesktop.Platform.Locale/x86_64/25.08";
    private static final String COMMIT="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    public static void main(String[] args) throws Exception {
        File out=new File(args[0]); Files.createDirectories(out.toPath());
        JSONObject app=row(APP,"/app"), runtime=row(RUNTIME,"/usr");
        JSONObject plan=new JSONObject().put("version",1).put("appRef",APP).put("appCommit",COMMIT)
                .put("origin","fixture").put("deployments",new JSONArray().put(app).put(runtime));

        // Reproduce the former call: a plan row is not the manifest consumed by stagedDeployment.
        try {
            DeployManifest.stagedDeployment(new File(out,"staging"),app);
            throw new AssertionError("row unexpectedly accepted as package manifest");
        } catch (org.json.JSONException expected) {
            if (!expected.getMessage().contains("appRef")) throw expected;
            System.out.println("PASS regression: InstallTransaction row fails stagedDeployment (missing appRef)");
        }
        JSONObject packageManifest=InstallHandoff.packageManifest(plan,APP);
        require(packageManifest,"appRef","appCommit","origin","dependencies","mount","deployments");
        if(!APP.equals(packageManifest.getString("appRef"))||!COMMIT.equals(packageManifest.getString("appCommit")))
            throw new AssertionError("wrong app identity at package boundary");
        File installation=new File(out,"staging/installation");
        File deployment=new File(installation,APP+"/"+COMMIT);
        Files.createDirectories(new File(deployment,"files").toPath());
        if(!DeployManifest.stagedDeployment(installation,packageManifest).equals(deployment))
            throw new AssertionError("package manifest resolved the wrong staged deployment");
        // StagedPacks reads dependencies from each plan row and pack metadata from this package manifest.
        JSONObject runtimePackage=InstallHandoff.packageManifest(plan,RUNTIME);
        require(runtimePackage,"appRef","appCommit","origin","dependencies","mount","deployments");
        runtimePackage.getJSONArray("dependencies"); runtimePackage.getString("mount");
        packageManifest.getJSONArray("deployments").getJSONObject(0).getBoolean("reused");
        System.out.println("PASS transaction and StagedPacks consumer JSON fields");

        // Reproduce a real Flatpak subpath deploy: completion must produce a row
        // that carries /en through package handoff to the <commit>-en directory.
        File localeInstallation=new File(out,"locale/installation");
        File localeDeployment=new File(localeInstallation,LOCALE+"/"+COMMIT+"-en");
        Files.createDirectories(localeDeployment.toPath());
        byte[] localeMetadata="[Runtime]\nname=Locale\n".getBytes(StandardCharsets.UTF_8);
        Files.write(new File(localeDeployment,"metadata").toPath(),localeMetadata);
        Files.write(new File(localeDeployment,"deploy").toPath(),
                DeployRecord.subpaths("fixture",COMMIT,Collections.singletonList("/en")));
        JSONObject deployed=DeployManifest.build(localeInstallation,LOCALE,"fixture","fixture-op");
        DependencyPlan.Info info=new DependencyPlan.Info(COMMIT,localeMetadata,"",Collections.emptyList());
        DependencyPlan.Node localeNode=new DependencyPlan.Node(LOCALE,"fixture",info,false);
        localeNode.subpaths.add("/en");
        PlanExecutor.Outcome<Boolean> pull=PlanExecutor.execute(Collections.singletonList(localeNode),1,node->{
            DeployManifest.verifyPlanned(localeInstallation,deployed,node.ref,node.info.commit,node.info.metadata,node.subpaths);
            return true;
        });
        if(!pull.completed.containsKey(LOCALE)||!pull.failed.isEmpty())
            throw new AssertionError("valid subpath deployment did not complete its pull node");
        JSONObject localeRowPlan=new JSONObject().put("version",1).put("appRef",LOCALE).put("appCommit",COMMIT)
                .put("origin","fixture").put("deployments",InstallHandoff.completedRows(
                        Collections.singletonList(localeNode),pull.completed.keySet(),
                        Collections.singletonMap(LOCALE,"[Runtime]"),Collections.singletonMap(LOCALE,"/usr"),LOCALE));
        JSONObject localePackage=InstallHandoff.packageManifest(localeRowPlan,LOCALE);
        if(!localePackage.getJSONArray("subpaths").getString(0).equals("/en"))
            throw new AssertionError("subpath was lost at package handoff");
        File resolved=DeployManifest.stagedDeployment(localeInstallation,localePackage);
        JSONObject localeIdentity=InstallHandoff.deploymentManifest(localeRowPlan,LOCALE);
        if(!resolved.equals(localeDeployment)||!resolved.equals(DeployManifest.stagedDeployment(localeInstallation,deployed))
                ||!resolved.equals(DeployManifest.stagedDeployment(localeInstallation,localeIdentity)))
            throw new AssertionError("subpath deploy resolved to the wrong directory");
        System.out.println("PASS subpath pull completion, transaction row, package handoff, and deployedir resolution");

        // Chrome's isolated extra-data pull uses --no-deploy. Before the
        // returned image is handed back there is no deploy record to scan;
        // materialize the verified commit, record its identity, then use the
        // ordinary manifest and planned-deployment validation path.
        String extraApp="app/com.google.Chrome/x86_64/stable";
        File extraInstallation=new File(out,"extra-data/installation");
        String extraCommit="abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
        byte[] extraMetadata="[Application]\nname=Chrome\n".getBytes(StandardCharsets.UTF_8);
        File extraDeployment=new File(extraInstallation,extraApp+"/"+extraCommit);
        Files.createDirectories(new File(extraDeployment,"files/extra").toPath());
        Files.write(new File(extraDeployment,"metadata").toPath(),extraMetadata);
        Files.write(new File(extraDeployment,"files/extra/returned-image.marker").toPath(),new byte[]{1});
        try {
            DeployManifest.build(extraInstallation,extraApp,"fixture","extra-op");
            throw new AssertionError("--no-deploy pull unexpectedly had a deploy manifest");
        } catch (java.io.IOException expected) {
            if(!expected.getMessage().contains("authorized application was not deployed"))throw expected;
        }
        DeployManifest.recordVerifiedPull(extraInstallation,extraApp,"fixture",extraCommit,
                Collections.emptyList(),extraMetadata);
        JSONObject extraManifest=DeployManifest.build(extraInstallation,extraApp,"fixture","extra-op");
        DependencyPlan.Info extraInfo=new DependencyPlan.Info(extraCommit,extraMetadata,"",Collections.emptyList());
        DependencyPlan.Node extraNode=new DependencyPlan.Node(extraApp,"fixture",extraInfo,false);
        DeployManifest.verifyPlanned(extraInstallation,extraManifest,extraNode.ref,extraNode.info.commit,
                extraNode.info.metadata,extraNode.subpaths);
        if(!extraCommit.equals(extraManifest.getString("appCommit"))
                ||!extraDeployment.equals(DeployManifest.stagedDeployment(extraInstallation,extraManifest)))
            throw new AssertionError("verified --no-deploy commit was not carried into the staged manifest");
        System.out.println("PASS extra-data handback, verified no-deploy checkout, and manifest commit handoff");

        String appName=DeployManifest.packageForPublic(APP), runtimeName=DeployManifest.packageForPublic(RUNTIME);
        if(!frameworkName(appName)||!frameworkName(runtimeName)) throw new AssertionError("invalid framework package name");
        // ef97ca4's transformation left a numeric branch segment beginning with a digit.
        String oldName="flatpak.runtime.org_example_Platform.x86_64._25_08.x3e32d97bd5de";
        if(frameworkName(oldName)) throw new AssertionError("legacy numeric package segment accepted");
        System.out.println("PASS AOSP package-name rule; legacy numeric branch rejected");

        // Run the real binary manifest/APK writer, replacing only Android Keystore signing.
        StubGenerator.setSigningBackendForTest((input,output)->Files.copy(input.toPath(),output.toPath(),
                java.nio.file.StandardCopyOption.REPLACE_EXISTING));
        File apk=new File(out,"handoff.apk");
        List<String> manyPermissions=new java.util.ArrayList<>();
        for(int i=0;i<300;i++)manyPermissions.add("org.example.permission.P"+i);
        StubGenerator.generatePackage(out,APP,"fixture","Editor",new byte[]{1},manyPermissions,
                appName,apk,null,Collections.emptyList(),new LinkedHashMap<>(),true,"/app");
        assertResourcesTableAligned(apk);
        File payload=new File(out,"carrier-payload");Files.write(payload.toPath(),new byte[]{1,2,3});
        ObjectPacks.Entry row=new ObjectPacks.Entry("/file",COMMIT,"file",0100644);row.source=payload;
        File packDir=new File(out,"carrier-packs");ObjectPacks.initial(Collections.singletonList(row),packDir,4096);
        File carrier=new File(out,"carrier.apk");LinkedHashMap<String,byte[]> appended=new LinkedHashMap<>();
        appended.put("AndroidManifest.xml",new byte[]{1});appended.put("resources.arsc",new byte[]{1,2,3,4});
        ObjectPacks.carrier(new File(packDir,row.pack),carrier,appended);
        assertResourcesTableAligned(carrier);
        System.out.println("Generated APK: "+apk.getAbsolutePath());
    }

    private static void assertResourcesTableAligned(File apk) throws Exception {
        try(RandomAccessFile zip=new RandomAccessFile(apk,"r")) {
            long offset=0;boolean found=false;
            while(offset+30<=zip.length()) {
                zip.seek(offset);int signature=readInt(zip);
                if(signature!=0x04034b50)break;
                zip.seek(offset+8);int method=readShortLE(zip);
                zip.seek(offset+18);long compressed=Integer.toUnsignedLong(readInt(zip));
                zip.seek(offset+26);
                int nameLength=readShortLE(zip),extraLength=readShortLE(zip);
                zip.seek(offset+30);
                byte[] name=new byte[nameLength];zip.readFully(name);
                long dataOffset=offset+30+nameLength+extraLength;
                if("resources.arsc".equals(new String(name,java.nio.charset.StandardCharsets.US_ASCII))) {
                    if(method!=0||dataOffset%4!=0)throw new AssertionError("resources.arsc must be STORED and four-byte aligned");
                    found=true;
                }
                offset=dataOffset+compressed;
            }
            if(!found)throw new AssertionError("APK omitted resources.arsc");
        }
    }
    private static int readInt(RandomAccessFile input) throws Exception {
        return input.readUnsignedByte()|input.readUnsignedByte()<<8|input.readUnsignedByte()<<16|input.readUnsignedByte()<<24;
    }
    private static int readShortLE(RandomAccessFile input) throws Exception {
        return input.readUnsignedByte()|input.readUnsignedByte()<<8;
    }

    private static JSONObject row(String ref,String mount) throws Exception {
        return new JSONObject().put("ref",ref).put("commit",COMMIT).put("origin","fixture")
                .put("metadata","[Application]\\nname=Fixture\\n").put("mount",mount).put("location","")
                .put("reused",false).put("dependencies",new JSONArray()).put("subpaths",new JSONArray());
    }
    private static void require(JSONObject object,String... keys) throws Exception {
        for(String key:keys) if(!object.has(key)) throw new AssertionError("missing consumer field "+key);
    }
    /** Port of FrameworkParsingPackageUtils.validateName(name, true, true). */
    private static boolean frameworkName(String name) {
        if(name.length()>255)return false;
        boolean sep=false,front=true;
        for(int i=0;i<name.length();i++) {
            char c=name.charAt(i);
            if((c>='a'&&c<='z')||(c>='A'&&c<='Z')){front=false;continue;}
            if(!front&&((c>='0'&&c<='9')||c=='_'))continue;
            if(c=='.'){sep=true;front=true;continue;}
            return false;
        }
        return sep;
    }
}
