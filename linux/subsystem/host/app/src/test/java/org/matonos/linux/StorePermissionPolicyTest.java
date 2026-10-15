package org.matonos.linux;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class StorePermissionPolicyTest {
    @Test public void packageOperationsRequireConfirmationUnlessPrivileged() {
        assertEquals(StorePermissionPolicy.Decision.CONFIRM,
                StorePermissionPolicy.packageOperation(false));
        assertEquals(StorePermissionPolicy.Decision.ALLOW,
                StorePermissionPolicy.packageOperation(true));
    }

    @Test public void remotesRequireConfirmationUnlessPrivileged() {
        assertEquals(StorePermissionPolicy.Decision.CONFIRM,
                StorePermissionPolicy.remoteMutation(false));
        assertEquals(StorePermissionPolicy.Decision.ALLOW,
                StorePermissionPolicy.remoteMutation(true));
    }

    @Test public void queriesRequireQueryOrInstallPermission() {
        assertEquals(StorePermissionPolicy.Decision.DENY,
                StorePermissionPolicy.query(false, false));
        assertEquals(StorePermissionPolicy.Decision.ALLOW,
                StorePermissionPolicy.query(true, false));
        assertEquals(StorePermissionPolicy.Decision.ALLOW,
                StorePermissionPolicy.query(false, true));
    }

    @Test public void operationOwnershipIsUidScoped() {
        assertTrue(StorePermissionPolicy.ownsOperation(10123, 10123));
        assertFalse(StorePermissionPolicy.ownsOperation(10123, 10124));
    }
}
