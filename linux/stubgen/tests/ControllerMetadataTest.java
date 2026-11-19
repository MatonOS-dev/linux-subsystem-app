import org.matonos.linuxhost.stubgen.StubGenerator;

public final class ControllerMetadataTest {
    private static void check(String metadata, boolean allowed) throws Exception {
        boolean requested = StubGenerator.permissionsForMetadata(metadata)
                .contains(StubGenerator.GAME_CONTROLLERS);
        if (requested != allowed) throw new AssertionError(metadata);
    }
    public static void main(String[] args) throws Exception {
        java.util.List<String> baseline = StubGenerator.permissionsForMetadata("");
        if (!baseline.equals(StubGenerator.templatePermissions())) throw new AssertionError("stub template permissions missing from common mapper: " + baseline);
        check("[Context]\ndevices=all;\n", true);
        if (!StubGenerator.permissionsForMetadata("[Context]\ndevices=all;\n").containsAll(StubGenerator.templatePermissions())) throw new AssertionError("template permissions missing for mapped stub");
        check("[Context]\ndevices=dri;input;\n", true);
        String av = "[Context]\nsockets=pulseaudio;wayland;\ndevices=all;\n";
        java.util.List<String> avPermissions = StubGenerator.permissionsForMetadata(av);
        if (!avPermissions.contains(StubGenerator.RECORD_AUDIO) || !avPermissions.contains(StubGenerator.CAMERA)
                || !avPermissions.contains(StubGenerator.GAME_CONTROLLERS))
            throw new AssertionError("AV/controller metadata mapping missing: " + avPermissions);
        java.util.List<String> negated = StubGenerator.permissionsForMetadata(
                "[Context]\nsockets=!pulseaudio;\ndevices=!all;\n");
        if (negated.contains(StubGenerator.RECORD_AUDIO) || negated.contains(StubGenerator.CAMERA))
            throw new AssertionError("negated metadata granted AV permission: " + negated);
        java.util.List<String> pipewire = StubGenerator.permissionsForMetadata("[Context]\nsockets=pipewire;\n");
        if (!pipewire.contains(StubGenerator.RECORD_AUDIO)) throw new AssertionError("pipewire audio mapping missing");
        check(" [Context] \r\n devices = input ; \r\n", true);
        check("[Context]\ndevices=dri;kvm;\n", false);
        check("[Other]\ndevices=all;\n", false);
        check("[Context]\ndevices=small;inputs;!input;\n", false);
        check("[Context]\n#devices=all;\ndevices=dri;\n", false);
        check("[Context]\ndevices=all;\ndevices=dri;\n", false);
        check("[Context]\ndevices=input;\n[Other]\ndevices=dri;\n", true);
        System.out.println("Controller metadata checks passed");
    }
}
