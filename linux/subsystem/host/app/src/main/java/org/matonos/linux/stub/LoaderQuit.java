package org.matonos.linux.stub;

/** Requests termination through the loader so its normal exit path owns cleanup. */
final class LoaderQuit {
    private LoaderQuit() {}

    static boolean request(Process loader, boolean alreadyExited) {
        if (loader == null || alreadyExited) return false;
        loader.destroy();
        return true;
    }
}
