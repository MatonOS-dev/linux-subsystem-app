package org.matonos.linux;

import static org.junit.Assert.*;
import org.junit.Test;

public final class StubSelfInstallIdentityTest {
    private static final byte[] CERT={1,2,3};
    private static final String REF="app/org.mozilla.firefox/x86_64/stable";

    @Test public void acceptsSolePackageWithSubsystemStubCertificate() {
        assertTrue(StubSelfInstallIdentity.matches(10123,10123,new String[]{"flatpak.org.mozilla.firefox"},new byte[][]{CERT},CERT));
    }
    @Test public void rejectsUidPackageOrSignerMismatch() {
        assertFalse(StubSelfInstallIdentity.matches(10124,10123,new String[]{"stub"},new byte[][]{CERT},CERT));
        assertFalse(StubSelfInstallIdentity.matches(10123,10123,new String[]{"stub","other"},new byte[][]{CERT},CERT));
        assertFalse(StubSelfInstallIdentity.matches(10123,10123,new String[]{"stub"},new byte[][]{{4}},CERT));
        assertFalse(StubSelfInstallIdentity.matches(10123,10123,new String[]{"stub"},new byte[][]{CERT,CERT},CERT));
    }
    @Test public void onlyAcceptsFlatpakApplicationRefs() {
        assertTrue(StubSelfInstallIdentity.validRef(REF));
        assertFalse(StubSelfInstallIdentity.validRef(null));
        assertFalse(StubSelfInstallIdentity.validRef("runtime/org.mozilla.firefox/x86_64/stable"));
        assertFalse(StubSelfInstallIdentity.validRef("app/../x86_64/stable"));
        assertFalse(StubSelfInstallIdentity.validRef("app/org.example/app/x86_64/stable"));
    }
}
