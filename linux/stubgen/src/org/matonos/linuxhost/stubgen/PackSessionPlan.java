package org.matonos.linuxhost.stubgen;

import java.io.IOException;
import java.io.File;
import java.util.*;

/** Android-independent planning for one atomic base + content-split session.
 * Callers must obtain installed splits/version from PackageManager, and desired
 * carriers from the new index (never from an unfiltered scratch-directory list).
 */
public final class PackSessionPlan {
    public enum Mode { FULL, INHERIT }
    public final Mode mode;
    public final long versionCode;
    public final List<String> write, remove;

    private PackSessionPlan(Mode mode, long version, boolean baseChanges,
            SortedSet<String> writes, SortedSet<String> removes) {
        this.mode=mode; versionCode=version;
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
        if(installed && !baseChanges && nextVersion!=installedVersion)
            throw new IOException("Inherited sessions require the installed versionCode; " +
                    "increasing versionCode requires a full install of every split");
        if(!installed && !baseChanges)throw new IOException("First install requires base.apk");
        SortedSet<String> prior=names(installedSplits), desired=names(desiredSplits);
        SortedSet<String> available=names(availableSplits), writes=new TreeSet<>(), removes=new TreeSet<>();
        if(!installed && !prior.isEmpty())throw new IOException("Unexpected installed splits");
        boolean full=!installed || baseChanges;
        if(full)writes.add("base.apk");
        for(String split:desired) {
            if(full || !prior.contains(split)) {
                if(!available.contains(split))throw new IOException("Missing new carrier: "+split);
                writes.add(split+".apk");
            }
        }
        if(!full)for(String split:prior)if(!desired.contains(split))removes.add(split);
        return new PackSessionPlan(full?Mode.FULL:Mode.INHERIT,nextVersion,baseChanges,writes,removes);
    }

    public static int nextRevision(long installedVersion,long nextVersion,int base,int[] splits) throws IOException {
        if(installedVersion!=nextVersion)return 1;
        int revision=base;
        if(base<0)throw new IOException("Invalid base revision");
        if(splits!=null)for(int value:splits) {
            if(value<0)throw new IOException("Invalid split revision");
            revision=Math.max(revision,value);
        }
        if(revision==Integer.MAX_VALUE)throw new IOException("Split revision exhausted; requires a new stub format version");
        return revision+1;
    }

    private static SortedSet<String> names(Collection<String> names) throws IOException {
        SortedSet<String> result=new TreeSet<>();
        for(String name:names) {
            if(name==null || !name.matches("(?:pack|index|extra|launcher|erofs)_[0-9a-f]{64}") || !result.add(name))
                throw new IOException("Invalid or duplicate content split: "+name);
        }
        return result;
    }

    public static boolean sameBase(File a,File b) throws Exception {
        try(java.util.zip.ZipFile left=new java.util.zip.ZipFile(a);java.util.zip.ZipFile right=new java.util.zip.ZipFile(b)) {
            java.util.Set<String> names=new java.util.TreeSet<>();
            for(java.util.Enumeration<? extends java.util.zip.ZipEntry> entries=left.entries();entries.hasMoreElements();) {
                String name=entries.nextElement().getName();if(!name.startsWith("META-INF/"))names.add(name);
            }
            java.util.Set<String> other=new java.util.TreeSet<>();
            for(java.util.Enumeration<? extends java.util.zip.ZipEntry> entries=right.entries();entries.hasMoreElements();) {
                String name=entries.nextElement().getName();if(!name.startsWith("META-INF/"))other.add(name);
            }
            if(!names.equals(other))return false;
            for(String name:names)try(java.io.InputStream x=left.getInputStream(left.getEntry(name));java.io.InputStream y=right.getInputStream(right.getEntry(name))) {
                if(!java.util.Arrays.equals(x.readAllBytes(),y.readAllBytes()))return false;
            }
            return true;
        }
    }
    /** A fakeable installer seam. An Android adapter owns file streaming/fsync,
     * callback delivery and abandonment after asynchronous commit failure.
     */
    public interface Installer {
        void begin(Mode mode) throws Exception;
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
            installer.begin(mode);begun=true;
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
