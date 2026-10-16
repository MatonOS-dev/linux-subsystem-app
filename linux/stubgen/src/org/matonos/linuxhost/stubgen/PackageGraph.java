package org.matonos.linuxhost.stubgen;

import java.io.IOException;
import java.util.*;

/** Mounted graph from signed records, shared by launch and extra-data execution. */
public final class PackageGraph {
    public interface Installed {
        /** Implementations must authenticate the package signer before returning a record. */
        DeploymentRecord read(String ref)throws Exception;
    }
    public static final class Entry {
        public final DeploymentRecord record;public final String mount;
        Entry(DeploymentRecord record,String mount){this.record=record;this.mount=mount;}
    }
    public static List<Entry> resolve(DeploymentRecord root,Installed installed)throws Exception {
        List<Entry> result=new ArrayList<>();result.add(new Entry(root,root.mount));
        Map<String,String> seen=new HashMap<>();seen.put(root.ref,root.mount);
        ArrayDeque<DeploymentRecord.Dependency> pending=new ArrayDeque<>(root.dependencies);
        while(!pending.isEmpty()) {
            DeploymentRecord.Dependency dependency=pending.removeFirst();
            String previous=seen.putIfAbsent(dependency.ref,dependency.mount);
            if(previous!=null){if(!previous.equals(dependency.mount))throw new IOException("Conflicting package mounts");continue;}
            if(result.size()>=128)throw new IOException("Too many declared packages");
            DeploymentRecord record=installed.read(dependency.ref);
            if(record==null||!record.ref.equals(dependency.ref))throw new IOException("Declared package ref mismatch");
            result.add(new Entry(record,dependency.mount));pending.addAll(record.dependencies);
        }
        return List.copyOf(result);
    }
    private PackageGraph(){}
}
