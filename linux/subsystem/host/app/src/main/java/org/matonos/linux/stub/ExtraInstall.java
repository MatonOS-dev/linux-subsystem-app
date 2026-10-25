package org.matonos.linux.stub;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

import org.matonos.linuxhost.stubgen.StubGenerator;

/**
 * Drives one package's {@link ExtraService} from the subsystem app.
 *
 * <p>The subsystem app installs a package's content (packs, no applied extra
 * output), then binds the package's own service, which runs {@code apply_extra}
     * under the package's UID, and receives its unsigned {@code extra_} APK
     * over a pipe. The subsystem app only validates its signing identity and signs it. The package is not
 * running yet: the base stub has no launcher entry until this step succeeds.
 *
 * <p>The pipe is the whole protocol: the service owns the write end and writes
 * {@link ExtraProtocol}'s status byte (and, on success, the APK). A service
 * that crashes is an ordinary truncation, and the watchdog closes the read end
 * so a stuck vendor script cannot pin the worker forever.
 */
public final class ExtraInstall {
    /** How long to wait for the whole extra step; the service refuses a
     * fraction earlier, so a clean error usually wins over our timeout. */
    public static final long TIMEOUT_MS = 15 * 60 * 1000;

    private ExtraInstall() { }

    /**
     * Applies {@code pkg}'s extra data and stages/signs its split APK in
     * {@code destination}. Returns {@code destination}, or {@code null} when
     * the package has no extra data at all ({@link ExtraProtocol#STATUS_NONE}).
     * Any failure is an {@link IOException} and leaves no installed split: the
     * caller removes a first install or regenerates on the next update.
     */
    public static File apply(Context context, String pkg, File destination) throws Exception {
        return apply(context, pkg, destination, TIMEOUT_MS);
    }

    public static File apply(Context context, String pkg, File destination, long timeoutMs) throws Exception {
        Intent intent = new Intent().setClassName(pkg, StubGenerator.EXTRA_SERVICE);
        final CountDownLatch connected = new CountDownLatch(1);
        final AtomicReference<IExtraExec> service = new AtomicReference<>();
        final AtomicReference<String> failure = new AtomicReference<>();
        ServiceConnection connection = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder binder) {
                service.set(IExtraExec.Stub.asInterface(binder));
                connected.countDown();
            }
            @Override public void onServiceDisconnected(ComponentName name) {
                failure.set("The package's extra service disconnected");
                connected.countDown();
            }
        };
        if (!context.bindService(intent, connection, Context.BIND_AUTO_CREATE))
            throw new IOException("Cannot connect to the package's extra service");
        try {
            if (!connected.await(timeoutMs, TimeUnit.MILLISECONDS))
                throw new IOException("Timed out connecting to the package's extra service");
            if (failure.get() != null) throw new IOException(failure.get());
            IExtraExec extra = service.get();
            if (extra == null) throw new IOException("The package declares no extra service");
            return read(extra, pkg, destination, timeoutMs);
        } finally {
            try { context.unbindService(connection); } catch (Exception ignored) { }
        }
    }

    /** One request: hand the service the pipe's write end, then read the reply. */
    private static File read(IExtraExec extra, String pkg, File destination, long timeoutMs) throws Exception {
        ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createPipe();
        final AtomicBoolean timedOut = new AtomicBoolean();
        ScheduledExecutorService watchdog = Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, "extra-install-timeout");
            thread.setDaemon(true);
            return thread;
        });
        ScheduledFuture<?> kill = watchdog.schedule(() -> {
            timedOut.set(true);
            try { pipe[0].close(); } catch (Exception ignored) { }
        }, timeoutMs, TimeUnit.MILLISECONDS);
        try {
            // The service handles the request on its own thread and returns at
            // once; a blocking binder call would deadlock on the pipe buffer.
            extra.applyExtra(pipe[1]);
        } finally {
            try { pipe[1].close(); } catch (IOException ignored) { }
        }
        try {
            try (InputStream in = new ParcelFileDescriptor.AutoCloseInputStream(pipe[0])) {
                int status = in.read();
                if (status < 0) throw truncated(timedOut);
                if (status == ExtraProtocol.STATUS_NONE) return null;
                if (status == ExtraProtocol.STATUS_ERROR) throw new IOException(message(in));
                if (status != ExtraProtocol.STATUS_OK) throw new IOException("Unexpected extra service reply");
                String packageName = readString(in), splitName = readString(in);
                int versionCode = readInt(in); long length = readLong(in);
                if (!packageName.equals(pkg) || versionCode != StubGenerator.STUB_VERSION_CODE ||
                        !splitName.matches("extra_[0-9a-f]{64}") || length < 0 ||
                        length > destination.getParentFile().getUsableSpace())
                    throw new IOException("Invalid extra split identity or insufficient staging space");
                java.nio.file.Files.createDirectories(destination.getParentFile().toPath());
                File unsigned = new File(destination.getParentFile(), splitName + ".unsigned.apk");
                File signed = new File(destination.getParentFile(), splitName + ".apk");
                try {
                    try (FileOutputStream out = new FileOutputStream(unsigned)) {
                        byte[] buffer = new byte[65536]; long remaining = length;
                        while (remaining > 0) {
                            int count = in.read(buffer, 0, (int)Math.min(buffer.length, remaining));
                            if (count < 0) throw new IOException("Truncated extra APK stream");
                            out.write(buffer, 0, count); remaining -= count;
                        }
                    }
                    if (!StubGenerator.validExtraIdentity(unsigned, packageName, splitName))
                        throw new IOException("Extra APK manifest identity mismatch");
                    StubGenerator.signExtra(unsigned, signed);
                } finally { unsigned.delete(); }
                if (!signed.renameTo(new File(destination.getParentFile(), destination.getName()))) {
                    File target=new File(destination.getParentFile(),destination.getName());
                    java.nio.file.Files.move(signed.toPath(),target.toPath(),java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                    return target;
                }
                return new File(destination.getParentFile(),destination.getName());
            } catch (IOException error) {
                if (timedOut.get()) throw new IOException("The package's apply_extra timed out", error);
                throw error;
            }
        } finally {
            kill.cancel(false);
            watchdog.shutdownNow();
        }
    }

    private static String readString(InputStream in) throws IOException {
        int length = readInt(in);
        if (length <= 0) throw new IOException("Invalid extra identity field length");
        byte[] bytes = new byte[length]; int offset = 0;
        while (offset < length) { int count = in.read(bytes, offset, length-offset); if (count < 0) throw new IOException("Truncated identity"); offset += count; }
        return new String(bytes, java.nio.charset.StandardCharsets.UTF_8);
    }
    private static int readInt(InputStream in) throws IOException {
        int a=in.read(),b=in.read(),c=in.read(),d=in.read();
        if ((a|b|c|d)<0) throw new IOException("Truncated extra response");
        return (a<<24)|(b<<16)|(c<<8)|d;
    }
    private static long readLong(InputStream in) throws IOException {
        long value=0; for(int i=0;i<8;i++){int b=in.read();if(b<0)throw new IOException("Truncated extra response");value=(value<<8)|(b&255);} return value;
    }

    private static IOException truncated(AtomicBoolean timedOut) {
        return timedOut.get()
                ? new IOException("The package's apply_extra timed out")
                : new IOException("The package's extra service returned no result");
    }

    /** {@link ExtraProtocol#STATUS_ERROR}: a four-byte length then a message. */
    private static String message(InputStream in) throws IOException {
        byte[] header = new byte[4];
        for (int i = 0; i < header.length; i++) {
            int value = in.read();
            if (value < 0) throw new IOException("Truncated extra service error");
            header[i] = (byte) value;
        }
        int length = ((header[0] & 255) << 24) | ((header[1] & 255) << 16)
                | ((header[2] & 255) << 8) | (header[3] & 255);
        if (length < 0)
            throw new IOException("Invalid extra service error length");
        byte[] text = new byte[length];
        int read = 0;
        while (read < length) {
            int count = in.read(text, read, length - read);
            if (count < 0) throw new IOException("Truncated extra service error");
            read += count;
        }
        String message = new String(text, java.nio.charset.StandardCharsets.UTF_8).trim();
        return message.isEmpty() ? "apply_extra failed" : message;
    }
}
