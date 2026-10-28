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
 * <p>The loader is the active same-signer host's
 * {@code libmatonos-v3-loader.so}; PackageManager resolves the installed host. The
 * resolver view is shared so app launches and installs cannot drift apart.
 */
final class LoaderCommand {
    static final String FLATPAK_TOOL = "libmatonos-flatpak.so";
    static final String FLATPAK_CLI = "libmatonos-app-flatpak.so";
    static final String FLATPAK_BWRAP = "libmatonos-app-bwrap.so";

    /** Loader for the calling package's host, resolved through PackageManager. */
    static File loader(Context context) throws PackageManager.NameNotFoundException {
        File loader = new File(nativeLibraryDir(context), "libmatonos-v3-loader.so");
        if (!loader.isFile() || !loader.canExecute())
            throw new IllegalStateException("Update the Linux host: v3 loader is missing");
        return loader;
    }

    /** Executable tools shipped beside the loader in this host APK. */
    static File tool(Context context, String name) throws PackageManager.NameNotFoundException {
        File tool = new File(nativeLibraryDir(context), name);
        if (!tool.isFile() || !tool.canExecute())
            throw new IllegalStateException("Update the Linux host: " + name + " is missing");
        return tool;
    }

    static File nativeLibraryDir(Context context) throws PackageManager.NameNotFoundException {
        ApplicationInfo host = context.getPackageManager().getApplicationInfo("org.matonos.linux", 0);
        return new File(host.nativeLibraryDir);
    }

    /** Resolver address intercepted by the compat proxy for this process. */
    static String resolverAddress() {
        return "127.0.0.53";
    }

    /** The resolv.conf a loader tree maps over /etc/resolv.conf. */
    static File writeResolv(File resolv, String dns) throws Exception {
        Files.write(resolv.toPath(), ("nameserver " + dns + "\n").getBytes(StandardCharsets.UTF_8));
        return resolv;
    }

    /** Enables per-process DNS interception for the configured resolver. */
    static void addDns(List<String> command, String dns) {
        command.add("--dns-forwarder"); command.add(dns + ":53");
    }

    /** Supplies the Linux host resolver and stable identity that Flatpak binds into sandboxes. */
    static void addResolvMaps(Context context, List<String> command, File resolv) throws Exception {
        File machineId = MachineId.ensure(context.getFilesDir());
        for (String target : new String[]{"/etc/machine-id", "/system/etc/machine-id",
                "/var/lib/dbus/machine-id"}) {
            command.add("--map"); command.add(machineId.getAbsolutePath()); command.add(target);
        }
        // On the host /etc is a symlink to /system/etc. The VFS walks that
        // ancestor before selecting the leaf map. Flatpak's SessionHelper
        // fallback tests this canonical host path, then binds it into the app.
        command.add("--map"); command.add(resolv.getAbsolutePath()); command.add("/etc/resolv.conf");
        command.add("--map"); command.add(resolv.getAbsolutePath()); command.add("/system/etc/resolv.conf");
    }

    private LoaderCommand() { }
}
