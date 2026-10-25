package org.matonos.linux.stub;

import org.json.JSONObject;

/** Typed boundary between the transaction plan rows and per-package consumers. */
public final class InstallHandoff {
    private InstallHandoff() { }

    /** Converts one transaction row into the package manifest expected by DeployManifest/StagedPacks. */
    public static JSONObject packageManifest(JSONObject plan, String ref) throws Exception {
        org.json.JSONArray rows=plan.getJSONArray("deployments");JSONObject row=findRow(plan,ref);
        return new JSONObject().put("appRef", ref).put("appCommit", row.getString("commit"))
                .put("origin", row.getString("origin")).put("dependencies", row.getJSONArray("dependencies"))
                .put("mount", row.getString("mount")).put("subpaths", row.getJSONArray("subpaths"))
                .put("deployments", rows);
    }

    /** Compact ref identity used when consumers need a staged deployment path. */
    public static JSONObject deploymentManifest(JSONObject plan,String ref)throws Exception {
        JSONObject row=findRow(plan,ref);
        return new JSONObject().put("appRef",ref).put("appCommit",row.getString("commit"))
                .put("subpaths",row.getJSONArray("subpaths"));
    }

    private static JSONObject findRow(JSONObject plan,String ref)throws Exception {
        org.json.JSONArray rows=plan.getJSONArray("deployments");
        for(int i=0;i<rows.length();i++) {
            JSONObject row=rows.getJSONObject(i);
            if(ref.equals(row.getString("ref")))return row;
        }
        throw new java.io.IOException("Missing planned package: " + ref);
    }

    /** Builds the persisted rows from the actual pull outcome; subpaths identify Flatpak's deployedir. */
    public static org.json.JSONArray completedRows(
            java.util.List<org.matonos.linuxhost.stubgen.DependencyPlan.Node> plan,
            java.util.Set<String> completed, java.util.Map<String,String> metadata,
            java.util.Map<String,String> mounts, String appRef) throws Exception {
        org.json.JSONArray rows=new org.json.JSONArray();
        for(org.matonos.linuxhost.stubgen.DependencyPlan.Node node:plan) {
            if(!completed.contains(node.ref))continue;
            org.json.JSONArray dependencies=new org.json.JSONArray();
            for(String ref:node.dependencies)dependencies.put(ref);
            org.json.JSONArray stageDependencies=new org.json.JSONArray();
            for(String ref:node.stageDependencies)stageDependencies.put(ref);
            org.json.JSONArray subpaths=new org.json.JSONArray();
            for(String path:node.subpaths)subpaths.put(path);
            String location=node.ref.equals(appRef)?"":"package-"+plan.indexOf(node);
            rows.put(new JSONObject().put("ref",node.ref).put("commit",node.info.commit).put("origin",node.origin)
                    .put("metadata",metadata.get(node.ref)).put("mount",mounts.get(node.ref)).put("location",location)
                    .put("reused",node.reused).put("dependencies",dependencies)
                    .put("stageDependencies",stageDependencies).put("subpaths",subpaths));
        }
        return rows;
    }
}
