package org.matonos.linux;

import java.security.MessageDigest;

/** Pure identity rules for a generated stub asking the host store to install its own ref. */
final class StubSelfInstallIdentity {
    private StubSelfInstallIdentity() { }

    static boolean matches(int callerUid, int packageUid, String[] packages,
            byte[][] signers, byte[] expectedCertificate) {
        return callerUid >= 0 && callerUid == packageUid && packages != null && packages.length == 1
                && packages[0] != null && !packages[0].isEmpty()
                && signers != null && signers.length == 1 && signers[0] != null
                && expectedCertificate != null && expectedCertificate.length > 0
                && MessageDigest.isEqual(signers[0], expectedCertificate);
    }

    static boolean validRef(String ref) {
        if (ref == null || !ref.matches("app/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+")) return false;
        String[] parts=ref.split("/",-1);
        for (String part : parts) if (part.equals(".") || part.equals("..")) return false;
        return true;
    }
}
