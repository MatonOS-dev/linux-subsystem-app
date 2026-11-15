package org.matonos.linux.store;

import java.io.IOException;
import java.util.*;

/** Only schedules Android commits; Flatpak has already selected/resolved every ref. */
public final class CommitChain {
    public static final class Package {
        public final String name;
        public final List<String> dependencies;
        public final String[] names,remove;
        public final boolean inherit;
        public Package(String name,List<String> dependencies,boolean inherit,String[] names,String[] remove) {
            this.name=name;this.dependencies=new ArrayList<>(dependencies);this.inherit=inherit;
            this.names=names.clone();this.remove=remove.clone();
        }
    }
    public interface Sink {
        /** Must return only after the store's final session result, or throw on failure. */
        void commit(Package item) throws Exception;
        void done() throws Exception;
        void failed(Exception failure) throws Exception;
    }
    public static void run(List<Package> packages,Sink sink) throws Exception {
        try {
            Map<String,Package> pending=new LinkedHashMap<>();
            for(Package item:packages)if(pending.put(item.name,item)!=null)throw new IOException("Duplicate Android package "+item.name);
            Set<String> all=new HashSet<>(pending.keySet()),complete=new HashSet<>();
            // Validate the entire graph before opening the first session.
            List<Package> ordered=new ArrayList<>();
            while(!pending.isEmpty()) {
                boolean advanced=false;
                for(Package item:new ArrayList<>(pending.values())) {
                    if(!all.containsAll(item.dependencies))throw new IOException("Missing dependency of "+item.name+": "+item.dependencies);
                    if(!complete.containsAll(item.dependencies))continue;
                    ordered.add(item);complete.add(item.name);pending.remove(item.name);advanced=true;
                }
                if(!advanced)throw new IOException("Cyclic Android package dependencies: "+pending.keySet());
            }
            for(Package item:ordered)sink.commit(item);
            sink.done();
        }catch(Exception failure){sink.failed(failure);}
    }
    private CommitChain() { }
}
