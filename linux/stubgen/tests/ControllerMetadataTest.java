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
