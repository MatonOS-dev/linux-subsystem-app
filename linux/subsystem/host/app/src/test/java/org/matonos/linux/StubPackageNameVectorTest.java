package org.matonos.linux;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.fail;

import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.matonos.linux.stub.DeployManifest;

/** The store derives stub package names itself; this vector is the shared contract. */
public final class StubPackageNameVectorTest {
    @Test public void packageNamesMatchSharedVector() throws Exception {
        JSONObject vector;
        try (InputStream in = getClass().getClassLoader().getResourceAsStream("stub-package-names.json")) {
            vector = new JSONObject(new String(in.readAllBytes(), StandardCharsets.UTF_8));
        }
        JSONArray valid = vector.getJSONArray("valid");
        for (int i = 0; i < valid.length(); i++) {
            JSONObject entry = valid.getJSONObject(i);
            assertEquals(entry.getString("ref"), entry.getString("package"),
                    DeployManifest.packageForPublic(entry.getString("ref")));
            assertEquals(entry.getString("ref"), entry.getString("split"),
                    DeployManifest.splitForPublic(entry.getString("ref")));
        }
        JSONArray invalid = vector.getJSONArray("invalid");
        for (int i = 0; i < invalid.length(); i++) {
            try {
                DeployManifest.packageForPublic(invalid.getString(i));
                fail("accepted invalid ref " + invalid.getString(i));
            } catch (java.io.IOException expected) { }
        }
    }
}
