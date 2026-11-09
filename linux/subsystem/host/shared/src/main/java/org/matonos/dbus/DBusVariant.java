package org.matonos.dbus;

/** A value and its D-Bus signature, used for variant (v) values. */
public final class DBusVariant {
    public final String signature;
    public final Object value;
    public DBusVariant(String signature, Object value) { this.signature = signature; this.value = value; }
}
