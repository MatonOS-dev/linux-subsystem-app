package org.matonos.linux.stub;

import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.Set;

/** Tracks Android windows belonging to this one stub process. */
final class WindowLifetime {
    private final Set<Object> activities = Collections.newSetFromMap(new IdentityHashMap<>());

    synchronized void opened(Object activity) { activities.add(activity); }

    /** Recreation and closing one of several windows must keep the session. */
    synchronized boolean destroyed(Object activity, boolean finishing, boolean changingConfiguration) {
        return activities.remove(activity) && activities.isEmpty() && finishing && !changingConfiguration;
    }
}
