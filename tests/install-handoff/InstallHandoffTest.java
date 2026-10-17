package org.matonos.linuxhost.stubgen;

import java.io.File;
import java.nio.file.Files;
import java.util.Collections;
import java.util.LinkedHashMap;
import org.json.JSONArray;
import org.json.JSONObject;
import org.matonos.linux.stub.DeployManifest;
import org.matonos.linux.stub.InstallHandoff;

/** Host regression for the untyped transaction-row to package-manifest boundary. */
public final class InstallHandoffTest {
    private static final String APP="app/org.example.Editor/x86_64/stable";
    private static final String RUNTIME="runtime/org.example.Platform/x86_64/25.08";
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
        StubGenerator.generatePackage(out,APP,"fixture","Editor",new byte[]{1},Collections.emptyList(),
                appName,apk,null,Collections.emptyList(),new LinkedHashMap<>(),false,"/app");
        System.out.println("Generated APK: "+apk.getAbsolutePath());
    }

    private static JSONObject row(String ref,String mount) throws Exception {
        return new JSONObject().put("ref",ref).put("commit",COMMIT).put("origin","fixture")
                .put("metadata","[Application]\\nname=Fixture\\n").put("mount",mount).put("location","")
                .put("reused",false).put("dependencies",new JSONArray());
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
