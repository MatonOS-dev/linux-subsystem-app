package org.matonos.linux.stub;

import static org.junit.Assert.*;
import java.util.*;
import org.junit.Test;

public final class PackageMountsTest {
 @Test public void usesParentExtensionDirectory() throws Exception {
  String runtime="runtime/org.freedesktop.Platform/x86_64/26.08";
  String extension="runtime/org.freedesktop.Platform.Compat.i386/x86_64/26.08";
  assertEquals("/usr",PackageMounts.mountPoint(runtime,"[Runtime]\n",Collections.emptyMap()));
  assertEquals("/usr/lib/i386-linux-gnu",PackageMounts.mountPoint(extension,"[ExtensionOf]\nref="+runtime+"\n",Collections.singletonMap(runtime,"[Extension org.freedesktop.Platform.Compat.i386]\ndirectory=lib/i386-linux-gnu\nversion=26.08\n")));
  String app="app/org.example.Editor/x86_64/stable",appExtension="app/org.example.Editor.Plugin/x86_64/stable";
  assertEquals("/app/lib/editor/extensions",PackageMounts.mountPoint(appExtension,"[ExtensionOf]\nref="+app+"\n",Collections.singletonMap(app,"[Extension org.example.Editor.Plugin]\ndirectory=lib/editor/extensions\n")));
 }
 @Test public void extensionParentMayBePlannedInstalledOrMissing() throws Exception {
  String parent="runtime/org.freedesktop.Platform/x86_64/26.08";
  String extension="runtime/org.freedesktop.Platform.Locale/x86_64/26.08";
  String extensionMetadata="[ExtensionOf]\nref="+parent+"\n";
  String parentMetadata="[Extension org.freedesktop.Platform.Locale]\ndirectory=share/runtime/locale\n";
  assertEquals("/usr/share/runtime/locale",PackageMounts.mountPoint(extension,extensionMetadata,
    Collections.singletonMap(parent,parentMetadata)));
  assertEquals("/usr/share/runtime/locale",PackageMounts.mountPoint(extension,extensionMetadata,
    PackageMounts.metadataWithInstalled(Collections.emptyMap(),Collections.singletonMap(parent,parentMetadata))));
  try{PackageMounts.mountPoint(extension,extensionMetadata,Collections.emptyMap());fail("missing parent must fail clearly");}
  catch(java.io.IOException expected){assertTrue(expected.getMessage().contains(parent));}
 }
 @Test public void i386ExtensionsUseOnlyThePlannedX86_64ExtensionPoint() throws Exception {
  String runtime="runtime/org.freedesktop.Platform/x86_64/26.08";
  String compat="runtime/org.freedesktop.Platform.Compat.i386/x86_64/26.08";
  String compatMetadata="[ExtensionOf]\nref=runtime/org.freedesktop.Platform/i386/26.08\n";
  String app="app/com.valvesoftware.Steam/x86_64/stable";
  Map<String,String> staged=new HashMap<>();
  staged.put(runtime,"[Runtime]\nname=org.freedesktop.Platform\n");
  staged.put(app,"[Application]\nruntime=org.freedesktop.Platform/x86_64/26.08\n"+
    "[Extension org.freedesktop.Platform.Compat.i386]\ndirectory=lib/i386-linux-gnu\n"+
    "[Extension org.freedesktop.Platform.GL32]\ndirectory=lib/i386-linux-gnu/GL\n");
  assertEquals("/usr/lib/i386-linux-gnu",PackageMounts.mountPoint(compat,compatMetadata,staged));
  String gl32="runtime/org.freedesktop.Platform.GL32.default/x86_64/26.08";
  assertEquals("/usr/lib/i386-linux-gnu/GL",PackageMounts.mountPoint(gl32,compatMetadata,staged));
  try{PackageMounts.mountPoint(compat,compatMetadata,Collections.emptyMap());fail("must not invent the x86_64 parent ref");}
  catch(java.io.IOException expected){assertTrue(expected.getMessage().contains("runtime/org.freedesktop.Platform/i386/26.08"));}
 }
 @Test public void rejectsMissingAndEscapingDirectory() throws Exception {
  String ref="runtime/org.freedesktop.Platform.Codec/x86_64/26.08",parent="runtime/org.freedesktop.Platform/x86_64/26.08";
  try{PackageMounts.mountPoint(ref,"[ExtensionOf]\nref="+parent+"\n",Collections.singletonMap(parent,"[Extension org.freedesktop.Platform.Codec]\nversion=26.08\n"));fail();}catch(java.io.IOException expected){}
  try{PackageMounts.mountPoint(ref,"[ExtensionOf]\nref="+parent+"\n",Collections.singletonMap(parent,"[Extension org.freedesktop.Platform.Codec]\ndirectory=../../etc\n"));fail();}catch(java.io.IOException expected){}
 }
 @Test public void selectsGlExtensionBranchFromRuntime() throws Exception {
  String metadata="[Extension org.freedesktop.Platform.GL]\nversions = 26.08;26.08-extra;1.4\nversion = 1.4\n";
  assertEquals("26.08",PackageMounts.glExtensionBranch(metadata,"26.08"));
  assertEquals("25.08",PackageMounts.glExtensionBranch(
    "[Extension org.freedesktop.Platform.GL]\nversions = 25.08;25.08-extra;1.4\n","25.08"));
  assertEquals("26.08",PackageMounts.glExtensionBranch(metadata,"25.08"));
  assertEquals("1.4",PackageMounts.glExtensionBranch("[Extension org.freedesktop.Platform.GL]\nversion=1.4\n","24.08"));
  try{PackageMounts.glExtensionBranch("[Runtime]\nname=org.freedesktop.Platform\n","25.08");fail();}
  catch(java.io.IOException expected){assertTrue(expected.getMessage().contains("no org.freedesktop.Platform.GL extension point"));}
 }
 @Test public void launchTargetsUseFlatpakVersionOrderAndExtensionIdSuffixes() throws Exception {
  String runtime="runtime/org.freedesktop.Platform/x86_64/26.08";
  String app="app/com.valvesoftware.Steam/x86_64/stable";
  String gl="runtime/org.freedesktop.Platform.GL.default/x86_64/26.08";
  String glExtra="runtime/org.freedesktop.Platform.GL.default/x86_64/26.08-extra";
  String glMaton="runtime/org.freedesktop.Platform.GL.maton/x86_64/26.08-extra";
  String gl32="runtime/org.freedesktop.Platform.GL32.default/x86_64/26.08";
  String gl32Extra="runtime/org.freedesktop.Platform.GL32.default/x86_64/26.08-extra";
  String compat="runtime/org.freedesktop.Platform.Compat.i386/x86_64/26.08";
  String codecs="runtime/org.freedesktop.Platform.codecs-extra/x86_64/26.08-extra";
  String codecsWrong="runtime/org.freedesktop.Platform.codecs-extra/x86_64/26.08";
  String platformMetadata;
  try(java.io.InputStream input=getClass().getResourceAsStream("/platform-26.08.metadata")) {
   assertNotNull("real Platform 26.08 metadata fixture",input);
   platformMetadata=new String(input.readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
  }
  String origin="flathub",commit=org.matonos.linuxhost.stubgen.ObjectPacks.hex(
    java.security.MessageDigest.getInstance("SHA-256").digest(new byte[]{1}));
  java.util.List<String> refs=java.util.Arrays.asList(gl,glExtra,glMaton,gl32,gl32Extra,compat,codecs,codecsWrong);
  java.util.List<org.matonos.linuxhost.stubgen.DeploymentRecord.Dependency> runtimeDeps=new java.util.ArrayList<>();
  for(String ref:refs)runtimeDeps.add(new org.matonos.linuxhost.stubgen.DeploymentRecord.Dependency(ref,origin,"/usr"));
  java.util.List<org.matonos.linuxhost.stubgen.DeploymentRecord> records=new java.util.ArrayList<>();
  records.add(new org.matonos.linuxhost.stubgen.DeploymentRecord(app,commit,origin,
    ("[Application]\nruntime=org.freedesktop.Platform/x86_64/26.08\n").getBytes(),new byte[]{1},new byte[]{1},
    java.util.Collections.singletonList(new org.matonos.linuxhost.stubgen.DeploymentRecord.Dependency(runtime,origin,"/usr"))));
  records.add(new org.matonos.linuxhost.stubgen.DeploymentRecord(runtime,commit,origin,platformMetadata.getBytes(),new byte[]{1},new byte[]{1},runtimeDeps));
  for(String ref:refs)records.add(new org.matonos.linuxhost.stubgen.DeploymentRecord(ref,commit,origin,
    ("[ExtensionOf]\nref="+runtime+"\n").getBytes(),new byte[]{1},new byte[]{1}));
  java.util.Map<String,String> targets=PackageMounts.launchTargets(records);
  assertEquals("/usr/lib/x86_64-linux-gnu/GL/default",targets.get(gl));
  assertNull(targets.get(glExtra));
  assertEquals("/usr/lib/x86_64-linux-gnu/GL/maton",targets.get(glMaton));
  assertEquals("/usr/lib/i386-linux-gnu/GL/default",targets.get(gl32));
  assertNull(targets.get(gl32Extra));
  assertEquals("/usr/lib/i386-linux-gnu",targets.get(compat));
  assertEquals("/usr/lib/x86_64-linux-gnu/codecs-extra",targets.get(codecs));
  assertNull(targets.get(codecsWrong));
 }
}
