package org.matonos.linux.stub;

/**
 * Wire format between a package's {@code ExtraService} and the subsystem app.
 *
 * <p>One status byte leads the stream:
 *
 * <ul>
 *   <li>{@link #STATUS_OK}: package name, split name, versionCode, APK length,
 *       then the package-built unsigned APK.</li>
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
    /** Longest diagnostic message a failing service sends back. */
    public static final int MAX_MESSAGE_BYTES = 4096;

    private ExtraProtocol() { }
}
