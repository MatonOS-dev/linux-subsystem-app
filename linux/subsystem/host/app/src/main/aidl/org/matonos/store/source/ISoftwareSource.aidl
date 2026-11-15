package org.matonos.store.source;

import org.matonos.store.source.IInstallCallback;

/**
 * A software source (store backend). Implemented by plugin apps, called only by
 * the software centre. Data crosses as versioned JSON (see SOURCE-PLUGINS.md).
 * Keep the method order: it is the transaction order.
 */
interface ISoftwareSource {
    /** {"version":1,"name","label","capabilities":[...]}; see SOURCE-PLUGINS.md. */
    String info();
    /** JSON array of AppListing. */
    String search(String term);
    /** One AppDetails object. */
    String details(String id);
    /** JSON array of Review. */
    String reviews(String id);
    /** JSON array of AppListing for apps this source installed. */
    String installed();
    /** JSON array of AppListing with an update available. */
    String updates();
    /** JSON array of {"id","url","label","official","broken"}. */
    String repos();
    void addRepo(String url);
    void delRepo(String id);
    /** Prepares an install or update; the APKs come back through callback.apks. */
    void install(String id, String operationId, IInstallCallback callback);
}
