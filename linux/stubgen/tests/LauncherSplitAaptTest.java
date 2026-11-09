package org.matonos.linuxhost.stubgen;

import java.awt.image.BufferedImage;
import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.File;
import java.nio.file.Files;
import java.security.KeyFactory;
import java.security.PrivateKey;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import javax.imageio.ImageIO;
import com.android.apksig.ApkSigner;

/** Parses generated launcher splits with aapt2, including the optional monochrome entry. */
public final class LauncherSplitAaptTest {
    public static void main(String[] args) throws Exception {
        File outputDir = new File(args[1]);
        File workDir = new File(outputDir, "launcher-split-aapt");
        if (!workDir.mkdirs() && !workDir.isDirectory()) {
            throw new IllegalStateException("Cannot create launcher test directory: " + workDir);
        }

        byte[] icon = png(0xff336699);
        byte[] monochrome = png(0xffffffff);
        PrivateKey key = KeyFactory.getInstance("RSA").generatePrivate(
                new PKCS8EncodedKeySpec(Files.readAllBytes(new File(args[2]).toPath())));
        X509Certificate certificate;
        try (FileInputStream input = new FileInputStream(args[3])) {
            certificate = (X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(input);
        }
        StubGenerator.setSigningBackendForTest((input, signed) -> {
            ApkSigner.SignerConfig signer = new ApkSigner.SignerConfig.Builder("launcher-test", key,
                    Collections.singletonList(certificate)).build();
            new ApkSigner.Builder(Collections.singletonList(signer)).setInputApk(input).setOutputApk(signed)
                    .setV1SigningEnabled(false).setV2SigningEnabled(true).setV3SigningEnabled(true)
                    .setV4SigningEnabled(false).build().sign();
        });
        File withoutMonochrome = new File(workDir, "launcher-without-monochrome.apk");
        File withMonochrome = new File(workDir, "launcher-with-monochrome.apk");
        StubGenerator.generateLauncher(workDir, "org.matonos.test", "Test app",
                Collections.emptyList(), icon, withoutMonochrome, 1);
        StubGenerator.generateLauncher(workDir, "org.matonos.test", "Test app",
                Collections.emptyList(), icon, monochrome, withMonochrome, 1);

        String plainResources = dump(args[0], "resources", withoutMonochrome);
        String monochromeResources = dump(args[0], "resources", withMonochrome);
        String plainXml = dump(args[0], "xmltree", withoutMonochrome,
                "--file", "res/drawable/icon.xml");
        String monochromeXml = dump(args[0], "xmltree", withMonochrome,
                "--file", "res/drawable/icon.xml");

        if (plainResources.contains("drawable/monochrome")) {
            throw new AssertionError("plain launcher split unexpectedly has drawable/monochrome\n" + plainResources);
        }
        if (plainXml.contains("E: monochrome")) {
            throw new AssertionError("plain launcher icon.xml unexpectedly has a monochrome element\n" + plainXml);
        }
        if (!monochromeResources.contains("drawable/monochrome")) {
            throw new AssertionError("monochrome launcher split has no drawable/monochrome\n" + monochromeResources);
        }
        if (!monochromeXml.contains("E: monochrome") || !monochromeXml.contains("@0x7f020003")) {
            throw new AssertionError("icon.xml has no monochrome element referencing drawable/monochrome\n" + monochromeXml);
        }
        System.out.println("PASS AAPT2 parses launcher split resources with and without monochrome art");
    }

    private static byte[] png(int color) throws Exception {
        BufferedImage image = new BufferedImage(2, 2, BufferedImage.TYPE_INT_ARGB);
        for (int y = 0; y < image.getHeight(); y++) {
            for (int x = 0; x < image.getWidth(); x++) {
                image.setRGB(x, y, color);
            }
        }
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        if (!ImageIO.write(image, "png", bytes)) {
            throw new IllegalStateException("PNG writer is unavailable");
        }
        return bytes.toByteArray();
    }

    private static String dump(String aapt2, String command, File apk, String... options) throws Exception {
        java.util.List<String> arguments = new java.util.ArrayList<>();
        arguments.add(aapt2);
        arguments.add("dump");
        arguments.add(command);
        arguments.add(apk.getAbsolutePath());
        Collections.addAll(arguments, options);
        Process process = new ProcessBuilder(arguments).redirectErrorStream(true).start();
        byte[] output = process.getInputStream().readAllBytes();
        int status = process.waitFor();
        String text = new String(output, StandardCharsets.UTF_8);
        if (status != 0) {
            throw new AssertionError("aapt2 " + command + " failed for " + apk + " (exit " + status + "):\n" + text);
        }
        return text;
    }
}
