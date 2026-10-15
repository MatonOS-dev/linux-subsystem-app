package org.matonos.linux;

import android.net.LocalSocket;
import android.system.Os;

import java.io.EOFException;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** MBP1 framing over an Android LocalSocket, with SCM_RIGHTS descriptor passing. */
final class LocalSocketChannel implements PortalChannel {
    private final LocalSocket socket;
    private final InputStream in;
    private final OutputStream out;
    private final List<FileDescriptor> pending = new ArrayList<>();

    LocalSocketChannel(LocalSocket socket) throws IOException {
        this.socket = socket;
        this.in = socket.getInputStream();
        this.out = socket.getOutputStream();
    }

    @Override
    public Frame read() throws IOException {
        try {
            byte[] header = new byte[8];
            int read = in.read(header, 0, header.length);
            collect();
            if (read < 0) { closeDescriptors(pending); pending.clear(); return null; }
            while (read < header.length) {
                int n = in.read(header, read, header.length - read);
                collect();
                if (n < 0) throw new EOFException("Truncated D-Bus frame header");
                read += n;
            }
            int[] frame = PortalWire.validateFrameHeader(header);
            byte[] message = new byte[frame[0]];
            readFully(message);
            List<FileDescriptor> fds = new ArrayList<>(pending);
            if (fds.size() != frame[1]) throw new IOException("SCM_RIGHTS count does not match frame");
            pending.clear();
            return new Frame(message, fds);
        } catch (IOException | RuntimeException | Error error) {
            try { collect(); } catch (IOException cleanup) { error.addSuppressed(cleanup); }
            closeDescriptors(pending);
            pending.clear();
            throw error;
        }
    }

    @Override
    public void write(byte[] message, List<FileDescriptor> fds) throws IOException {
        if (message.length < 16 || message.length > PortalWire.MAX_MESSAGE)
            throw new IOException("Invalid D-Bus output size");
        List<FileDescriptor> descriptors = fds == null ? Collections.emptyList() : fds;
        if (descriptors.size() > PortalWire.MAX_FDS) throw new IOException("Too many descriptors");
        byte[] frame = ByteBuffer.allocate(8 + message.length).order(ByteOrder.LITTLE_ENDIAN)
                .putInt(message.length).putInt(descriptors.size()).put(message).array();
        socket.setFileDescriptorsForSend(descriptors.isEmpty() ? null : descriptors.toArray(new FileDescriptor[0]));
        try {
            out.write(frame);
        } finally {
            socket.setFileDescriptorsForSend(null);
        }
        out.flush();
    }

    @Override
    public void close() throws IOException {
        try { socket.close(); } finally { closeDescriptors(pending); pending.clear(); }
    }

    @Override
    public void closeDescriptors(List<FileDescriptor> descriptors) {
        if (descriptors == null) return;
        for (FileDescriptor descriptor : descriptors) {
            try {
                Os.close(descriptor);
            } catch (Exception ignored) {
                // Best effort: a descriptor already closed must not fail the loop.
            }
        }
    }

    private void readFully(byte[] buffer) throws IOException {
        int offset = 0;
        while (offset < buffer.length) {
            int n = in.read(buffer, offset, buffer.length - offset);
            collect();
            if (n < 0) throw new EOFException("Truncated D-Bus frame");
            offset += n;
        }
    }

    /** Descriptors accompany the first byte of a frame; gather them as we read. */
    private void collect() throws IOException {
        FileDescriptor[] descriptors = socket.getAncillaryFileDescriptors();
        if (descriptors != null) Collections.addAll(pending, descriptors);
    }
}
