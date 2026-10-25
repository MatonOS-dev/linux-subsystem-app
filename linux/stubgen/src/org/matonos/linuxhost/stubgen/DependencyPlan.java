package org.matonos.linuxhost.stubgen;

import java.io.IOException;
import java.util.*;

/** Coordinates upstream Flatpak inspection, installed-package reuse and origin choice.
 * Metadata interpretation and related-ref selection belong to the Flatpak adapter. */
public final class DependencyPlan {
    public static final class Info {
        public final String commit,runtime,parent;
        public final byte[] metadata;
        public final List<String> related;
        public final boolean mayHaveRelated;
        public Info(String commit,byte[] metadata,String runtime,List<String> related)throws IOException {
            this(commit,metadata,runtime,"",related,true);
        }
        public Info(String commit,byte[] metadata,String runtime,List<String> related,boolean mayHaveRelated)throws IOException {
            this(commit,metadata,runtime,"",related,mayHaveRelated);
        }
        public Info(String commit,byte[] metadata,String runtime,String parent,List<String> related,boolean mayHaveRelated)throws IOException {
            this.mayHaveRelated=mayHaveRelated;
            if(commit==null||!commit.matches("[0-9a-f]{64}")||metadata==null||metadata.length==0||metadata.length>128*1024)
                throw new IOException("Invalid Flatpak inspection result");
            this.commit=commit;this.metadata=metadata.clone();this.runtime=runtime;this.parent=parent;
            this.related=List.copyOf(refsOnly(related));
        }
    }
    public interface Backend {
        DeploymentRecord installed(String ref)throws Exception;
        /** Null only for Flatpak's explicit ref-not-found error; other errors must propagate. */
        Info inspect(String ref,String origin)throws Exception;
        String recordedOrigin(String ref)throws Exception;
        default Info recordedInfo(DeploymentRecord record)throws Exception{return new Info(record.commit,record.metadata,"",Collections.emptyList(),false);}
        default List<String> related(String ref,String origin,Info info)throws Exception{return info.related;}
        String choose(String ref,List<String> origins)throws Exception;
    }
    public static final class Node {
        public final String ref,origin;
        public final Info info;
        public final boolean reused;
        public final List<String> dependencies=new ArrayList<>();
        /** Required inputs to fetch before this ref; related refs are only staged alongside it. */
        public final List<String> stageDependencies=new ArrayList<>();
        public Node(String ref,String origin,Info info,boolean reused){this.ref=ref;this.origin=origin;this.info=info;this.reused=reused;}
    }
    private final Backend backend;
    private final List<String> origins;
    private final LinkedHashMap<String,Node> nodes=new LinkedHashMap<>();
    private final List<Node> order=new ArrayList<>();
    public DependencyPlan(String appOrigin,Collection<String> known,Backend backend)throws IOException {
        this.backend=backend;
        LinkedHashSet<String> ordered=new LinkedHashSet<>();ordered.add(appOrigin);ordered.add("flathub");ordered.addAll(known);
        for(String origin:ordered)RemoteOrigin.parse(origin);
        origins=List.copyOf(ordered);
    }
    public List<Node> build(String root,String origin,boolean repair)throws Exception {
        if(!nodes.isEmpty())throw new IllegalStateException("A plan is single-use");
        visit(root,origin,true,repair);
        return List.copyOf(order);
    }
    /** The plan helper appends subpaths to related refs ("ref\n/en", Locale with a language subset);
     *  planning uses the ref, Flatpak applies the subpaths at deploy. */
    public static List<String> refsOnly(List<String> related){List<String> refs=new ArrayList<>();for(String r:related){int nl=r.indexOf('\n');refs.add(nl<0?r:r.substring(0,nl));}return refs;}
    private Node visit(String ref,String requested,boolean root,boolean repair)throws Exception {
        if(!validRef(ref))throw new IOException("Invalid dependency ref: "+ref);
        Node existing=nodes.get(ref);if(existing!=null)return existing;
        if(nodes.size()>=128)throw new IOException("Too many dependency packages");
        DeploymentRecord installed=backend.installed(ref);
        DeploymentRecord recorded=(!root||repair)?installed:null;
        String origin;Info info;
        if(recorded!=null) {
            origin=recorded.remote;
            info=backend.recordedInfo(recorded);
        } else {
            origin=root?requested:(repair?(requested!=null?requested:backend.recordedOrigin(ref)):null);
            if(origin!=null) {
                info=backend.inspect(ref,origin);
                if(info==null)throw new IOException("Recorded origin no longer provides "+ref);
            } else {
                LinkedHashMap<String,Info> providers=new LinkedHashMap<>();
                for(String candidate:origins){Info found=backend.inspect(ref,candidate);if(found!=null)providers.put(candidate,found);}
                if(providers.isEmpty())throw new IOException("No known remote provides "+ref);
                origin=providers.size()==1?providers.keySet().iterator().next():backend.choose(ref,List.copyOf(providers.keySet()));
                info=providers.get(origin);if(info==null)throw new IOException("Dependency origin selection cancelled");
            }
        }
        Node node=new Node(ref,origin,info,!root&&installed!=null);nodes.put(ref,node);
        // Related Flatpak extensions can belong to a second runtime. Pull that
        // parent too so apply_extra has its authenticated metadata available.
        if(info.parent!=null&&!info.parent.isEmpty()) {
            node.dependencies.add(info.parent);node.stageDependencies.add(info.parent);visit(info.parent,null,false,repair);
        }
        if(recorded!=null) {
            for(DeploymentRecord.Dependency dependency:recorded.dependencies){
                node.dependencies.add(dependency.ref);node.stageDependencies.add(dependency.ref);visit(dependency.ref,dependency.remote,false,true);
            }
            if(info.runtime!=null&&!info.runtime.isEmpty()) {
                String runtime=info.runtime.startsWith("runtime/")?info.runtime:"runtime/"+info.runtime;
                if(!node.dependencies.contains(runtime)){node.dependencies.add(runtime);node.stageDependencies.add(runtime);visit(runtime,null,false,true);}
            }
            order.add(node);
            if(info.mayHaveRelated&&(root||recorded.dependencies.isEmpty()))for(String related:refsOnly(backend.related(ref,origin,info)))if(!node.dependencies.contains(related)) {
                node.dependencies.add(related);visit(related,null,false,true);
            }
        } else {
            // Match Flatpak's runtime-before-app ordering. Related extensions may
            // need their parent's runtime for apply_extra, so stage the parent first.
            if(info.runtime!=null&&!info.runtime.isEmpty()) {
                String runtime=info.runtime.startsWith("runtime/")?info.runtime:"runtime/"+info.runtime;
                node.dependencies.add(runtime);node.stageDependencies.add(runtime);visit(runtime,null,false,repair);
            }
            order.add(node);
            for(String related:refsOnly(backend.related(ref,origin,info)))if(!node.dependencies.contains(related)) {
                node.dependencies.add(related);visit(related,null,false,repair);
            }
        }
        return node;
    }
    public static boolean validRef(String ref) {
        if(ref==null||ref.length()>1024||!ref.matches("(?:app|runtime)/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+"))return false;
        for(String part:ref.split("/"))if(part.equals(".")||part.equals(".."))return false;
        return true;
    }
}
