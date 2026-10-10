import org.matonos.linuxhost.stubgen.StubGenerator;

public final class DesktopDisplayNameTest {
    public static void main(String[] args) throws Exception {
        String desktop = "[Desktop Entry]\nType=Application\nName=Kate\nName[de]=Kate DE\n"
                + "[Desktop Action new-window]\nName=New Window\n";
        if (!"Kate".equals(StubGenerator.desktopDisplayName(desktop)))
            throw new AssertionError("Must use the same primary Name as the generated manifest");
        if (!"LibreOffice".equals(StubGenerator.desktopDisplayName("[Desktop Entry]\nName=LibreOffice\n")))
            throw new AssertionError("Lost installed application display name");
        if (!"Linux application".equals(StubGenerator.desktopDisplayName("[Desktop Entry]\nName= \n")))
            throw new AssertionError("Missing-name fallback differs from the generator");
        try {
            StubGenerator.desktopDisplayName("[Other]\nName=Bad\n");
            throw new AssertionError("Accepted a non-desktop group");
        } catch (java.io.IOException expected) { }
        System.out.println("PASS: installed desktop labels, action groups and missing-name handling");
    }
}
