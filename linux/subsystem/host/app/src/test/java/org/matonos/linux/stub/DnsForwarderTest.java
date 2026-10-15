package org.matonos.linux.stub;

/** Tests the exact decision applied to the kernel's SO_PEERCRED UID. */
public final class DnsForwarderTest {
    public static void main(String[] args) {
        if (!DnsForwarder.allowedPeer(10372, 10372)) throw new AssertionError("Own UID rejected");
        if (DnsForwarder.allowedPeer(10373, 10372)) throw new AssertionError("Other app UID accepted");
        if (DnsForwarder.allowedPeer(0, 10372)) throw new AssertionError("Root bypassed app identity");
        if (DnsForwarder.allowedPeer(110372, 10372)) throw new AssertionError("Other user accepted");
        System.out.println("PASS DNS peer UID: own app only, rejects other app/root/user");
    }
}
