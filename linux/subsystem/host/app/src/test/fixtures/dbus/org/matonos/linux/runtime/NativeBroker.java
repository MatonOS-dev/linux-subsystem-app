package org.matonos.linux.runtime;
public class NativeBroker {
    public static int starts,stops;
    public static boolean nativeStart(String socket,String policy,String portal,String secret){starts++;return true;}
    public static void nativeStop(){stops++;}
}
