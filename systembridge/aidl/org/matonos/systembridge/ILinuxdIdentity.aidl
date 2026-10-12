package org.matonos.systembridge;

/** Registered only by the authenticated System Bridge; queried live, never cached. */
interface ILinuxdIdentity {
    String query(String command, int uid, String packageName);
}
