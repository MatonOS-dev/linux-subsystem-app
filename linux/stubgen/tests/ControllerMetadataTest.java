import org.matonos.linuxhost.stubgen.StubGenerator;

public final class ControllerMetadataTest {
    private static void check(String metadata, boolean allowed) throws Exception {
        boolean requested = StubGenerator.permissionsForMetadata(metadata)
                .contains(StubGenerator.GAME_CONTROLLERS);
        if (requested != allowed) throw new AssertionError(metadata);
    }
    private static void checkCode(String metadata, boolean allowed) throws Exception {
        boolean requested = StubGenerator.permissionsForMetadata(metadata)
                .contains(StubGenerator.RUN_DOWNLOADED_CODE);
        if (requested != allowed) throw new AssertionError(metadata);
    }
    public static void main(String[] args) throws Exception {
        check("[Context]\ndevices=all;\n", true);
        check("[Context]\ndevices=dri;input;\n", true);
        check(" [Context] \r\n devices = input ; \r\n", true);
        check("[Context]\ndevices=dri;kvm;\n", false);
        check("[Other]\ndevices=all;\n", false);
        check("[Context]\ndevices=small;inputs;!input;\n", false);
        check("[Context]\n#devices=all;\ndevices=dri;\n", false);
        check("[Context]\ndevices=all;\ndevices=dri;\n", false);
        check("[Context]\ndevices=input;\n[Other]\ndevices=dri;\n", true);
        // RUN_DOWNLOADED_CODE is default deny; only broad writable/persistent
        // filesystem patterns request it.
        checkCode("[Context]\ndevices=all;\n", false);
        checkCode("[Context]\nfilesystems=host;\n", true);
        checkCode("[Context]\nfilesystems=home:ro;!home;\n", true);
        checkCode("[Context]\nfilesystems=~/;\n", true);
        checkCode("[Context]\nfilesystems=~/Steam:create;\n", true);
        checkCode("[Context]\nfilesystems=/media;\n", false);
        checkCode("[Context]\nfilesystems=xdg-documents;\n", false);
        checkCode("[Context]\npersistent=~/.config/foo;\n", true);
        checkCode("[Context]\npersistent=;\n", false);
        checkCode("[Other]\nfilesystems=host;\n", false);
        System.out.println("Controller metadata checks passed");
    }
}
