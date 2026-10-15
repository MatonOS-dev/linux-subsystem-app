package org.matonos.linux;

import java.io.FileDescriptor;
import java.io.IOException;
import java.util.List;

/**
 * A framed D-Bus transport: the MBP1 header (eight bytes: little-endian payload
 * length and descriptor count) followed by one D-Bus message.
 */
interface PortalChannel extends AutoCloseable {
    final class Frame {
        final byte[] message;
        final List<FileDescriptor> fds;
        Frame(byte[] message, List<FileDescriptor> fds) { this.message = message; this.fds = fds; }
    }

    /** Read one frame, blocking. Returns null at end of stream. */
    Frame read() throws IOException;

    /** Write one frame. */
    void write(byte[] message, List<FileDescriptor> fds) throws IOException;

    /** Release descriptors received with a frame; the channel owns them. */
    void closeDescriptors(List<FileDescriptor> descriptors);

    @Override
    void close() throws IOException;
}
