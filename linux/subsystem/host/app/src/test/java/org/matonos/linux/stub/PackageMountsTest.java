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
}
