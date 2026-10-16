package org.matonos.linux.stub;

/**
 * Wire format between a package's {@code ExtraService} and the subsystem app.
 *
 * <p>One status byte leads the stream:
 *
 * <ul>
 *   <li>{@link #STATUS_OK}: a POSIX ustar archive of the applied extra tree
 *       follows (see {@link Tar}).</li>
 *   <li>{@link #STATUS_NONE}: the package has no extra data; the stream ends
 *       after this byte.</li>
 *   <li>{@link #STATUS_ERROR}: a four-byte big-endian length and a UTF-8
 *       message follow.</li>
 * </ul>
 *
 * <p>The stream is carried over a {@code ParcelFileDescriptor} pipe: the
 * service owns the write end and the subsystem app the read end, so a crashed
 * or killed service is an ordinary truncation, never a hung transaction.
 */
public final class ExtraProtocol {
    public static final int STATUS_OK = 0;
    public static final int STATUS_NONE = 1;
    public static final int STATUS_ERROR = 2;
    /** Largest applied extra tree the subsystem app accepts, in bytes. */
    public static final long MAX_OUTPUT_BYTES = 8L * 1024 * 1024 * 1024;
    /** Largest single file in the applied extra tree, in bytes. */
    public static final long MAX_ENTRY_BYTES = 4L * 1024 * 1024 * 1024;
    /** Longest diagnostic message a failing service sends back. */
    public static final int MAX_MESSAGE_BYTES = 4096;

    private ExtraProtocol() { }
}
