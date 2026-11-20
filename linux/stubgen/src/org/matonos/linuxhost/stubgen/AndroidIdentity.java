package org.matonos.linuxhost.stubgen;

import java.io.IOException;

/** Stable Android package and split identifiers for Flatpak refs. */
public final class AndroidIdentity {
    public static String packageName(String appId) throws IOException {
        return "flatpak." + identifier(appId);
    }

    public static String splitName(String arch, String branch) throws IOException {
        return component(arch) + "." + component(branch);
    }

    /** Maps Android-invalid characters to '_' and prefixes numeric segments with 'v'. */
    public static String identifier(String source) throws IOException {
        if (source == null || source.isEmpty()) throw new IOException("Empty Flatpak identifier");
        StringBuilder result = new StringBuilder();
        for (String segment : source.split("\\.", -1)) {
            if (segment.isEmpty()) throw new IOException("Empty Flatpak identifier segment");
            if (result.length() != 0) result.append('.');
            char first = segment.charAt(0);
            if (!letter(first)) result.append('v');
            for (int i = 0; i < segment.length(); i++) {
                char c = segment.charAt(i);
                result.append(letter(c) || digit(c) || c == '_' ? c : '_');
            }
        }
        return result.toString();
    }

    private static String component(String source) throws IOException {
        if(source==null||source.isEmpty())throw new IOException("Empty Flatpak identifier");
        StringBuilder result=new StringBuilder();
        if(!letter(source.charAt(0)))result.append('v');
        for(int i=0;i<source.length();i++) {
            char c=source.charAt(i);
            result.append(letter(c)||digit(c)||c=='_'?c:'_');
        }
        return result.toString();
    }

    private static boolean letter(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    private static boolean digit(char c) { return c >= '0' && c <= '9'; }
    private AndroidIdentity() { }
}
