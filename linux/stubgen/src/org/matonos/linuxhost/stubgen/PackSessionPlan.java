package org.matonos.linuxhost.stubgen;

import java.io.IOException;
import java.util.*;

/** Android-independent planning for one atomic base + content-split session.
 * Callers must obtain installed splits/version from PackageManager, and desired
 * carriers from the new index (never from an unfiltered scratch-directory list).
 */
public final class PackSessionPlan {
    public enum Mode { FULL, INHERIT }
    public final Mode mode;
    public final long versionCode;
    public final boolean dontKillApp;
    public final List<String> write, remove;

    private PackSessionPlan(Mode mode, long version, boolean baseChanges,
            SortedSet<String> writes, SortedSet<String> removes) {
        this.mode=mode; versionCode=version;
        // PackageInstallerSession clears INSTALL_DONT_KILL_APP whenever a base
        // is supplied. Pack index updates always supply a base.
        dontKillApp=mode==Mode.INHERIT && !baseChanges;
        write=Collections.unmodifiableList(new ArrayList<>(writes));
        remove=Collections.unmodifiableList(new ArrayList<>(removes));
    }

    public static PackSessionPlan create(boolean installed, long installedVersion,
            long nextVersion, Collection<String> installedSplits,
            Collection<String> desiredSplits, Collection<String> availableSplits,
            boolean baseChanges) throws IOException {
        if(nextVersion<1 || (installed && nextVersion<installedVersion))
            throw new IOException("Invalid or decreasing package version");
        // INHERIT checks Existing's version even when every split is replaced.
        if(installed && nextVersion!=installedVersion)
            throw new IOException("Inherited sessions require the installed versionCode; " +
                    "increasing versionCode requires a full install of every split");
        if(!installed && !baseChanges)throw new IOException("First install requires base.apk");
        SortedSet<String> prior=names(installedSplits), desired=names(desiredSplits);
        SortedSet<String> available=names(availableSplits), writes=new TreeSet<>(), removes=new TreeSet<>();
        if(!installed && !prior.isEmpty())throw new IOException("Unexpected installed splits");
        if(baseChanges)writes.add("base.apk");
        for(String split:desired) {
            if(!installed || !prior.contains(split)) {
                if(!available.contains(split))throw new IOException("Missing new carrier: "+split);
                writes.add(split+".apk");
            }
        }
        for(String split:prior)if(!desired.contains(split))removes.add(split);
        return new PackSessionPlan(installed?Mode.INHERIT:Mode.FULL,nextVersion,baseChanges,writes,removes);
    }

    private static SortedSet<String> names(Collection<String> names) throws IOException {
        SortedSet<String> result=new TreeSet<>();
        for(String name:names) {
            if(name==null || !name.matches("pack_[0-9a-f]{64}") || !result.add(name))
                throw new IOException("Invalid or duplicate content split: "+name);
        }
        return result;
    }

    /** A fakeable installer seam. An Android adapter owns file streaming/fsync,
     * callback delivery and abandonment after asynchronous commit failure.
     */
    public interface Installer {
        void begin(Mode mode, boolean dontKillApp) throws Exception;
        void write(String apk) throws Exception;
        void remove(String split) throws Exception;
        void commit() throws Exception;
        void abandon() throws Exception;
    }
    public void submit(Installer installer) throws Exception {
        submit(installer,()->false);
    }
    public void submit(Installer installer, java.util.function.BooleanSupplier cancelled) throws Exception {
        boolean begun=false;
        try {
            checkCancelled(cancelled);
            installer.begin(mode,dontKillApp);begun=true;
            for(String apk:write){checkCancelled(cancelled);installer.write(apk);}
            for(String split:remove){checkCancelled(cancelled);installer.remove(split);}
            checkCancelled(cancelled);
            installer.commit();
        } catch(Exception failure) {
            if(begun)try {installer.abandon();}catch(Exception cleanup){failure.addSuppressed(cleanup);}
            throw failure;
        }
    }
    private static void checkCancelled(java.util.function.BooleanSupplier cancelled) throws IOException {
        if(cancelled.getAsBoolean())throw new IOException("Install cancelled");
    }
}
