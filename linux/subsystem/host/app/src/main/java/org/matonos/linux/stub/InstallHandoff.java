package org.matonos.linux.stub;

import org.json.JSONObject;

/** Typed boundary between the transaction plan rows and per-package consumers. */
public final class InstallHandoff {
    private InstallHandoff() { }

    /** Converts one transaction row into the package manifest expected by DeployManifest/StagedPacks. */
    public static JSONObject packageManifest(JSONObject plan, String ref) throws Exception {
        org.json.JSONArray rows = plan.getJSONArray("deployments");
        for (int i = 0; i < rows.length(); i++) {
            JSONObject row = rows.getJSONObject(i);
            if (!ref.equals(row.getString("ref"))) continue;
            return new JSONObject().put("appRef", ref).put("appCommit", row.getString("commit"))
                    .put("origin", row.getString("origin")).put("dependencies", row.getJSONArray("dependencies"))
                    .put("mount", row.getString("mount")).put("deployments", rows);
        }
        throw new java.io.IOException("Missing planned package: " + ref);
    }
}
