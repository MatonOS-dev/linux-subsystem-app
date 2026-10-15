package org.matonos.linux;

import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.system.Os;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.io.FileDescriptor;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.UUID;

/** Android-runtime checks for LocalSocket APIs that cannot run in the host JVM suite. */
@RunWith(AndroidJUnit4.class)
public final class MatonLocalTransportInstrumentedTest {
    @Test
    public void testAcceptedSocketReportsPeerUid() throws Exception {
        String name = "maton-portal-test-" + UUID.randomUUID();
        try (LocalServerSocket server = new LocalServerSocket(name);
             LocalSocket client = new LocalSocket()) {
            client.connect(new LocalSocketAddress(name, LocalSocketAddress.Namespace.ABSTRACT));
            try (LocalSocket accepted = server.accept()) {
                org.junit.Assert.assertEquals(Process.myUid(), accepted.getPeerCredentials().getUid());
            }
        }
    }

    @Test
    public void testLocalSocketPassesOneScmRightsDescriptor() throws Exception {
        String name = "maton-portal-fd-" + UUID.randomUUID();
        try (LocalServerSocket server = new LocalServerSocket(name);
             LocalSocket client = new LocalSocket();
             ParcelFileDescriptor source = ParcelFileDescriptor.open(new File("/dev/null"), ParcelFileDescriptor.MODE_READ_ONLY)) {
            client.connect(new LocalSocketAddress(name, LocalSocketAddress.Namespace.ABSTRACT));
            try (LocalSocket accepted = server.accept()) {
                client.setFileDescriptorsForSend(new FileDescriptor[] { source.getFileDescriptor() });
                client.getOutputStream().write(new byte[] { 0x5a });
                org.junit.Assert.assertEquals(0x5a, accepted.getInputStream().read());
                FileDescriptor[] received = accepted.getAncillaryFileDescriptors();
                org.junit.Assert.assertNotNull(received);
                org.junit.Assert.assertEquals(1, received.length);
                Os.fstat(received[0]);
                Os.close(received[0]);
            }
        }
    }

    @Test
    public void testRejectsMalformedFrameHeaders() {
        byte[] badLength = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(15).putInt(0).array();
        try {
            PortalWire.validateFrameHeader(badLength);
            org.junit.Assert.fail("short D-Bus message length accepted");
        } catch (IllegalArgumentException expected) { }

        byte[] badDescriptorCount = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(16).putInt(17).array();
        try {
            PortalWire.validateFrameHeader(badDescriptorCount);
            org.junit.Assert.fail("excessive SCM_RIGHTS count accepted");
        } catch (IllegalArgumentException expected) { }
    }
}
