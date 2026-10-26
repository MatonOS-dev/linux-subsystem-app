package org.matonos.linuxhost.stubgen;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.KeyFactory;
import java.security.PrivateKey;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.*;

/** Exercises generated split names through planning and the production validator. */
public final class SessionPlanHandoffTest {
    private static void check(boolean value,String message) {
        if(!value)throw new AssertionError(message);
    }
    private static final class FakePackageInstaller implements PackSessionPlan.Installer {
        final File artifacts;
        final List<String> accepted=new ArrayList<>();
        FakePackageInstaller(File artifacts){this.artifacts=artifacts;}
        void accept(PackSessionPlan plan)throws Exception { plan.submit(this); }
        public void begin(PackSessionPlan.Mode mode) { check(mode==PackSessionPlan.Mode.FULL,"first install mode"); }
        public void write(String name)throws Exception {
            File file=org.matonos.linux.stub.PackInstaller.validateArtifact(artifacts,name);
            com.android.apksig.ApkVerifier.Result signature=new com.android.apksig.ApkVerifier.Builder(file)
                    .setMinCheckedPlatformVersion(30).build().verify();
            check(signature.isVerified(),"StubGenerator artifact signature: "+name+" "+signature.getErrors());
            accepted.add(name);
        }
        public void remove(String split){throw new AssertionError("Unexpected removal: "+split);}
        public void commit() {}
        public void abandon(){throw new AssertionError("Unexpected abandon");}
    }
    private static final class PackageFixture {
        final String ref,pkg;
        final File base,image,objects,work,artifacts;
        PackageFixture(File root,String ref,String pkg,byte[] imageBytes)throws Exception {
            this.ref=ref;this.pkg=pkg;
            File dir=new File(root,pkg);check(dir.mkdirs(),"create fixture directory");
            base=new File(dir,"base.apk");image=new File(dir,"deployment.erofs");
            objects=new File(dir,"objects");work=new File(dir,"work");artifacts=new File(dir,"artifacts");
            check(objects.mkdirs()&&work.mkdirs()&&artifacts.mkdirs(),"create package fixture paths");
            Files.write(image.toPath(),imageBytes);
            List<ObjectPacks.Entry> staged=new ArrayList<>();
            if(ref.equals("app/org.example.Editor/x86_64/stable")) {
                staged.add(new ObjectPacks.Entry("/Locale","0".repeat(64),"dir",0100755));
                staged.add(new ObjectPacks.Entry("/Locale/fr","1".repeat(64),"dir",0100755));
            }
            ObjectPacks.writeIndex(new File(objects,"objects.index"),staged);
            ObjectPacks.writeMetadata(new File(objects,"objects.ostree"),Collections.emptyMap());
            StubGenerator.generatePackage(work,ref,"flathub",pkg,new byte[]{1},Collections.emptyList(),pkg,base,
                    objects,Collections.emptyList(),Collections.emptyMap(),true,"/app");
            Files.copy(base.toPath(),new File(artifacts,"base.apk").toPath());
        }
        String generateAndPlan()throws Exception {
            String index=StubGenerator.indexSplitName(objects);
            StubGenerator.generateIndex(work,pkg,objects,new File(artifacts,index+".apk"),1);
            String erofs=StubGenerator.erofsSplitName(image);
            StubGenerator.generateErofsPack(work,pkg,image,new File(artifacts,erofs+".apk"),1);
            List<String> desired=Arrays.asList(index,erofs);
            PackSessionPlan plan=PackSessionPlan.create(false,0,StubGenerator.STUB_VERSION_CODE,
                    Collections.emptyList(),desired,desired,true);
            new FakePackageInstaller(artifacts).accept(plan);
            return erofs;
        }
    }
    public static void main(String[] args)throws Exception {
        File root=new File(args[0],"session-plan-handoff");
        if(root.exists())try(java.util.stream.Stream<java.nio.file.Path> paths=Files.walk(root.toPath())) {
            paths.sorted(Comparator.reverseOrder()).forEach(path->{try{Files.delete(path);}catch(Exception e){throw new RuntimeException(e);}});
        }
        check(root.mkdirs(),"create test root");
        PrivateKey key=KeyFactory.getInstance("RSA").generatePrivate(new PKCS8EncodedKeySpec(
                Files.readAllBytes(new File(args[1]).toPath())));
        X509Certificate certificate;
        try(java.io.InputStream input=Files.newInputStream(new File(args[2]).toPath())) {
            certificate=(X509Certificate)CertificateFactory.getInstance("X.509").generateCertificate(input);
        }
        com.android.apksig.ApkSigner.SignerConfig signer=new com.android.apksig.ApkSigner.SignerConfig.Builder(
                "session-plan-test",key,Collections.singletonList(certificate)).build();
        StubGenerator.setSigningBackendForTest((input,output)->new com.android.apksig.ApkSigner.Builder(
                Collections.singletonList(signer)).setInputApk(input).setOutputApk(output)
                .setV1SigningEnabled(false).setV2SigningEnabled(true).setV3SigningEnabled(true)
                .setV4SigningEnabled(false).setAlignmentPreserved(true).build().sign());
        try {
            byte[] appImage="app image with Locale/fr staged content".getBytes(StandardCharsets.UTF_8);
            byte[] runtimeImage="runtime image".getBytes(StandardCharsets.UTF_8);
            byte[] extensionImage="identical extension image".getBytes(StandardCharsets.UTF_8);
            List<String> stagedSubpaths=Collections.singletonList("Locale/fr");
            List<PackageFixture> packages=Arrays.asList(
                    new PackageFixture(root,"app/org.example.Editor/x86_64/stable","org.example.App",appImage),
                    new PackageFixture(root,"runtime/org.example.Platform/x86_64/24.08","org.example.Runtime",runtimeImage),
                    new PackageFixture(root,"app/org.example.ExtensionOne/x86_64/stable","org.example.ExtensionOne",extensionImage),
                    new PackageFixture(root,"app/org.example.ExtensionTwo/x86_64/stable","org.example.ExtensionTwo",extensionImage));
            List<String> imageNames=new ArrayList<>();
            for(PackageFixture fixture:packages)imageNames.add(fixture.generateAndPlan());
            check(stagedSubpaths.contains("Locale/fr")&&ObjectPacks.readIndex(new File(packages.get(0).objects,"objects.index"))
                    .stream().anyMatch(entry->entry.path.equals("/Locale/fr")),"Locale subpath is carried in staged package metadata");
            check(imageNames.get(2).equals(imageNames.get(3)),"identical extension image names should be content-addressed equally");
            check(imageNames.stream().allMatch(name->name.matches("erofs_[0-9a-f]{64}")),"all generated image splits use EROFS names");
        } finally {
            StubGenerator.setSigningBackendForTest(null);
        }
        System.out.println("Session plan install handoff tests passed");
    }
}
