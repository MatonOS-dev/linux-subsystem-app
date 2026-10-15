package org.matonos.linux.runtime;

/** Embedded per-app PipeWire/Pulse server and Android playback sink. */
public final class NativeAudio {
    static { System.loadLibrary("maton_pipewire"); }
    private NativeAudio() { }
    public static native boolean nativeStart(String runtimeDirectory, String configDirectory);
    public static native void nativeStop();
}
