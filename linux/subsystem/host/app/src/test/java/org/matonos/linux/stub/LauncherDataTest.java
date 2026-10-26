package org.matonos.linux.stub;

import static org.junit.Assert.*;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

import org.junit.Test;

/** The launcher label, MIME types and icon source order of a package. */
public final class LauncherDataTest {
    private static final String APP_ID = "org.example.Editor";

    @Test public void prefersTheDeployedExtraExport() throws Exception {
        Path extra = Files.createTempDirectory("launcher-extra");
        desktop(extra, APP_ID, "Extra App", "extra-icon", "text/plain;application/pdf;");
        png(extra.resolve("export/share/icons/hicolor/64x64/apps/extra-icon.png"));
        Path deployment = Files.createTempDirectory("launcher-deployment");
        desktop(deployment, APP_ID, "Deployed App", null, null);

        LauncherData data = LauncherData.resolve(deployment.toFile(), APP_ID, extra.toFile(), null);
        assertEquals("Extra App", data.label);
        assertEquals(2, data.mimeTypes.size());
        assertNotNull("The extra export carries the icon", data.icon);
    }

    @Test public void fromExtraOutputIsNullWithoutADesktop() throws Exception {
        Path extra = Files.createTempDirectory("launcher-extra-none");
        Files.createDirectories(extra.resolve("bin"));
        assertNull(LauncherData.fromExtraOutput(extra.toFile(), APP_ID));
    }

    @Test public void usesTheDeploymentExport() throws Exception {
        Path deployment = Files.createTempDirectory("launcher-deployed");
        desktop(deployment, APP_ID, "Deployed App", null, "image/png;");
        png(appearanceIcon(deployment, APP_ID));

        LauncherData data = LauncherData.resolve(deployment.toFile(), APP_ID, null, null);
        assertEquals("Deployed App", data.label);
        assertEquals(1, data.mimeTypes.size());
        assertNotNull(data.icon);
    }

    @Test public void fallsBackToAppstream() throws Exception {
        LauncherData data = LauncherData.resolve(null, APP_ID, null,
                appId -> new LauncherData.Appstream.Entry("From Appstream", png()));
        assertEquals("From Appstream", data.label);
        assertNotNull(data.icon);
    }

    @Test public void fallsBackToTheAppId() throws Exception {
        LauncherData data = LauncherData.resolve(null, APP_ID, null, null);
        assertEquals(APP_ID, data.label);
        assertNull("The caller supplies the generic icon", data.icon);
        assertTrue(data.mimeTypes.isEmpty());
    }

    @Test public void ignoresAnAppstreamEntryWithoutAName() throws Exception {
        LauncherData data = LauncherData.resolve(null, APP_ID, null,
                appId -> new LauncherData.Appstream.Entry("", png()));
        assertEquals(APP_ID, data.label);
        assertNull(data.icon);
    }

    @Test public void validIconAcceptsOnlyPng() {
        assertNotNull(LauncherData.validIcon(png()));
        assertNull(LauncherData.validIcon(null));
        assertNull(LauncherData.validIcon(new byte[]{1, 2, 3, 4, 5, 6, 7, 8}));
        byte[] large = new byte[1024 * 1024 + 1];
        System.arraycopy(png(), 0, large, 0, png().length);
        assertNotNull(LauncherData.validIcon(large));
    }

    @Test public void picksTheLargestExportedIcon() throws Exception {
        Path deployment = Files.createTempDirectory("launcher-sizes");
        desktop(deployment, APP_ID, "Sized App", APP_ID, null);
        png(appearanceIcon(deployment, APP_ID));
        byte[] small = png();
        byte[] large = new byte[64];
        System.arraycopy(png(), 0, large, 0, png().length);
        Files.write(appearanceIcon(deployment, APP_ID), small);
        Path largePath = deployment.resolve("export/share/icons/hicolor/256x256/apps").resolve(APP_ID + ".png");
        Files.createDirectories(largePath.getParent());
        Files.write(largePath, large);

        LauncherData data = LauncherData.resolve(deployment.toFile(), APP_ID, null, null);
        assertArrayEquals("The largest size wins", large, data.icon);
    }

    @Test public void ignoresASymlinkedExportedIcon() throws Exception {
        Path deployment = Files.createTempDirectory("launcher-symlink");
        desktop(deployment, APP_ID, "Linked App", APP_ID, null);
        Path icon = appearanceIcon(deployment, APP_ID);
        Files.createDirectories(icon.getParent());
        Files.createSymbolicLink(icon, Path.of("../../../../../../etc/hostname"));

        LauncherData data = LauncherData.resolve(deployment.toFile(), APP_ID, null, null);
        assertEquals("Linked App", data.label);
        assertNull("A symlinked icon is never read", data.icon);
    }

    @Test public void doesNotScanForAnUnrelatedIcon() throws Exception {
        Path deployment = Files.createTempDirectory("launcher-unrelated-icon");
        desktop(deployment, APP_ID, "No Icon", "missing-icon", null);
        png(deployment.resolve("export/share/icons/hicolor/64x64/apps/other-app.png"));

        LauncherData data = LauncherData.resolve(deployment.toFile(), APP_ID, null, null);
        assertNull("Only the desktop icon name and app id are candidates", data.icon);
    }

    private static Path appearanceIcon(Path root, String appId) {
        return root.resolve("export/share/icons/hicolor/64x64/apps").resolve(appId + ".png");
    }

    private static void desktop(Path root, String appId, String name, String icon, String mime)
            throws Exception {
        Path directory = Files.createDirectories(root.resolve("export/share/applications"));
        StringBuilder text = new StringBuilder("[Desktop Entry]\nType=Application\n");
        if (name != null) text.append("Name=").append(name).append('\n');
        if (icon != null) text.append("Icon=").append(icon).append('\n');
        if (mime != null) text.append("MimeType=").append(mime).append('\n');
        Files.write(directory.resolve(appId + ".desktop"),
                text.toString().getBytes(StandardCharsets.UTF_8));
    }

    private static void png(Path file) throws Exception {
        Files.createDirectories(file.getParent());
        Files.write(file, png());
    }

    private static byte[] png() {
        return new byte[]{(byte) 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 1, 2, 3, 4};
    }
}
