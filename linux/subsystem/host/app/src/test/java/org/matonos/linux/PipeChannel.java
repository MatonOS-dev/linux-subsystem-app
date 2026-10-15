package org.matonos.linux;

import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Collections;
import java.util.List;

/** Host JVM transport: MBP1 framing over streams, without descriptor passing. */
final class PipeChannel implements PortalChannel {
    private final InputStream in;
    private final OutputStream out;

    PipeChannel(InputStream in, OutputStream out) { this.in = in; this.out = out; }

    @Override
    public Frame read() throws IOException {
        byte[] header = readN(8);
        if (header == null) return null;
        int[] frame = PortalWire.validateFrameHeader(header);
        byte[] message = readN(frame[0]);
        if (message == null) throw new EOFException("Truncated D-Bus frame");
        if (frame[1] != 0) throw new IOException("Test channel cannot receive descriptors");
        return new Frame(message, Collections.emptyList());
    }

    @Override
    public void write(byte[] message, List<java.io.FileDescriptor> fds) throws IOException {
        if (fds != null && !fds.isEmpty()) throw new IOException("Test channel cannot send descriptors");
        out.write(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(message.length).putInt(0).array());
        out.write(message);
        out.flush();
    }

    @Override
    public void close() throws IOException {
        in.close();
        out.close();
    }

    @Override
    public void closeDescriptors(List<java.io.FileDescriptor> descriptors) {
        // The test transport carries no real descriptors.
    }

    private byte[] readN(int n) throws IOException {
        byte[] buffer = new byte[n];
        int offset = 0;
        while (offset < n) {
            int read = in.read(buffer, offset, n - offset);
            if (read < 0) {
                if (offset == 0) return null;
                throw new EOFException("Truncated D-Bus frame");
            }
            offset += read;
        }
        return buffer;
    }
}
