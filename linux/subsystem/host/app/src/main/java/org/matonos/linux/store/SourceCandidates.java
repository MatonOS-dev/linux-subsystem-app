package org.matonos.linux.store;
import java.io.IOException;
import java.util.*;
/** Candidate identity is a full Flatpak ref plus the actual remote name. */
public final class SourceCandidates {
    public static Map<String,Object> install(String id,String repo) {
        if(id==null||!id.matches("app/[A-Za-z0-9._+-]+/[A-Za-z0-9._+-]+/[A-Za-z0-9._+-]+"))throw new IllegalArgumentException("Invalid Flatpak app ref: "+id);
        if(repo==null||!repo.matches("[A-Za-z0-9_][A-Za-z0-9_.-]*"))throw new IllegalArgumentException("Invalid Flatpak remote: "+repo);
        return new LinkedHashMap<>(Map.of("id",id,"ref",id,"origin",repo));
    }
    public static Map<String,Object> find(List<Map<String,Object>> rows,String id,String repo)throws IOException {
        for(Map<String,Object> row:rows)if(Objects.equals(id,row.get("id"))&&(repo==null||Objects.equals(repo,row.get("origin"))))return new LinkedHashMap<>(row);
        throw new IOException("AppStream application missing id="+id+" repo="+repo);
    }
    private SourceCandidates(){}
}
