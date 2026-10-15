package org.matonos.linux.stub;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

/**
 * Shared construction of the v3-loader command for both an app launch
 * ({@link V3Launcher}) and a host Flatpak install ({@link InstallTransaction}).
 *
 * <p>The loader is the image or same-signer updated host's
 * {@code libmatonos-v3-loader.so}; PackageManager follows host updates. The
 * resolver view and the {@link DnsForwarder} lifecycle are owned here so the
 * two callers cannot drift apart.
 */
final class LoaderCommand {
    static final String FLATPAK_BIN = "/apex/com.matonos.flatpak/bin";

    /** Loader for the calling package's host, resolved through PackageManager. */
    static File loader(Context context) throws PackageManager.NameNotFoundException {
        ApplicationInfo host = context.getPackageManager().getApplicationInfo("org.matonos.linux", 0);
        File loader = new File(host.nativeLibraryDir, "libmatonos-v3-loader.so");
        if (!loader.isFile() || !loader.canExecute())
            throw new IllegalStateException("Update the Linux host: v3 loader is missing");
        return loader;
    }

    /** The logical resolver address this UID's forwarder answers on. */
    static String resolverAddress() {
        String dns = DnsForwarder.endpoint().replaceFirst(":53$", "");
        if (!dns.matches("127\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}"))
            throw new IllegalArgumentException("Invalid per-app DNS endpoint");
        return dns;
    }

    /** The resolv.conf a loader tree maps over /etc/resolv.conf. */
    static File writeResolv(File resolv, String dns) throws Exception {
        Files.write(resolv.toPath(), ("nameserver " + dns + "\n").getBytes(StandardCharsets.UTF_8));
        return resolv;
    }

    /** Binds this UID's forwarder in {@code runtime} and adds its arguments. */
    static void addDns(List<String> command, String dns, File runtime) throws Exception {
        String socket = DnsForwarder.acquire(runtime);
        if (socket == null || !DnsForwarder.endpoint().equals(dns + ":53"))
            throw new IllegalStateException("Per-app DNS forwarder is unavailable");
        command.add("--dns-forwarder"); command.add(dns + ":53");
        command.add("--dns-socket"); command.add(socket);
    }

    /** Maps the resolver view over both host locations of /etc/resolv.conf. */
    static void addResolvMaps(List<String> command, File resolv) {
        // On the host /etc is a symlink to /system/etc. The VFS walks that
        // ancestor before selecting the leaf map. Flatpak's SessionHelper
        // fallback tests this canonical host path, then binds it into the app.
        command.add("--map"); command.add(resolv.getAbsolutePath()); command.add("/etc/resolv.conf");
        command.add("--map"); command.add(resolv.getAbsolutePath()); command.add("/system/etc/resolv.conf");
    }

    private LoaderCommand() { }
}
