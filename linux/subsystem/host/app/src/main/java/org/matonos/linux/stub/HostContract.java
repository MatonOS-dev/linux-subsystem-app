package org.matonos.linux.stub;

/** Stable constants shared by generated Flatpak stubs and the host. */
public final class HostContract {
    public static final int INTERFACE_VERSION = 3;
    public static final String META_FLATPAK_REF = "org.matonos.linuxhost.FLATPAK_REF";
    public static final String META_MIN_INTERFACE = "org.matonos.linuxhost.MIN_INTERFACE_VERSION";
    public static final String EXTRA_FLATPAK_REF = "org.matonos.linuxhost.FLATPAK_REF";

    private HostContract() { }

    public static int getInterfaceVersion() { return INTERFACE_VERSION; }
}
