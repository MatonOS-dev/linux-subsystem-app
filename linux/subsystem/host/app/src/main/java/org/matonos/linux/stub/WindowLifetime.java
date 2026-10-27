package org.matonos.linux.stub;

import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.Set;

/** Tracks Android windows belonging to this one stub process. */
final class WindowLifetime {
    private final Set<Object> activities = Collections.newSetFromMap(new IdentityHashMap<>());

    synchronized void opened(Object activity) { activities.add(activity); }

    /** True only when a tracked activity was the last open Android window. */
    synchronized boolean destroyed(Object activity) {
        return activities.remove(activity) && activities.isEmpty();
    }
}
