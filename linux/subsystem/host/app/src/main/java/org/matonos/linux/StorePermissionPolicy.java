package org.matonos.linux;

/** Pure permission decisions for the exported Linux app store endpoint. */
final class StorePermissionPolicy {
    enum Decision { ALLOW, CONFIRM, DENY }

    static Decision packageOperation(boolean installPermission) {
        return installPermission ? Decision.ALLOW : Decision.CONFIRM;
    }

    static Decision remoteMutation(boolean installPermission) {
        return installPermission ? Decision.ALLOW : Decision.CONFIRM;
    }

    static Decision query(boolean queryPermission, boolean installPermission) {
        return queryPermission || installPermission ? Decision.ALLOW : Decision.DENY;
    }

    static boolean ownsOperation(int ownerUid, int callerUid) {
        return ownerUid == callerUid;
    }

    private StorePermissionPolicy() {}
}
