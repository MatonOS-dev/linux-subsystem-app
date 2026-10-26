package org.matonos.linuxhost.stubgen;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** Selects related extension refs using Flatpak's extension-point version order. */
public final class FlatpakExtensionSelection {
    private FlatpakExtensionSelection() {}

    /** Keep the first available configured version per extension name or subdirectory suffix. */
    public static List<String> select(String metadata, String parentRef, List<String> refs) throws IOException {
        if (metadata == null || refs == null) throw new IOException("Missing Flatpak extension selection input");
        Map<String, Map<String, String>> sections = parse(metadata);
        String parentBranch = branch(parentRef);
        Set<String> configuredCandidates = new LinkedHashSet<>();
        Set<String> selectedCandidates = new LinkedHashSet<>();
        for (Map.Entry<String, Map<String, String>> entry : sections.entrySet()) {
            if (!entry.getKey().startsWith("Extension ")) continue;
            String point = entry.getKey().substring("Extension ".length());
            Map<String, String> values = entry.getValue();
            boolean subdirectories = Boolean.parseBoolean(values.getOrDefault("subdirectories", "false"));
            Map<String, List<String>> bySuffix = new LinkedHashMap<>();
            for (String ref : refs) {
                String[] parts = ref.split("/", -1);
                if (parts.length != 4 || !parts[0].equals("runtime")) continue;
                String suffix = subdirectorySuffix(parts[1], point);
                if (suffix == null || (!suffix.isEmpty() && !subdirectories)) continue;
                configuredCandidates.add(ref);
                bySuffix.computeIfAbsent(suffix, ignored -> new ArrayList<>()).add(ref);
            }
            List<String> versions = versions(values, parentBranch);
            for (List<String> candidates : bySuffix.values()) {
                for (String version : versions) {
                    boolean found = false;
                    for (String candidate : candidates) {
                        if (branch(candidate).equals(version)) {
                            selectedCandidates.add(candidate);
                            found = true;
                        }
                    }
                    if (found) break;
                }
            }
        }
        List<String> result = new ArrayList<>();
        for (String ref : refs)
            if (!configuredCandidates.contains(ref) || selectedCandidates.contains(ref)) result.add(ref);
        return Collections.unmodifiableList(result);
    }

    /** Return the suffix Flatpak places below a subdirectories extension point. */
    public static String subdirectorySuffix(String extensionId, String point) {
        if (extensionId == null || point == null) return null;
        if (extensionId.equals(point)) return "";
        if (extensionId.startsWith(point + ".")) return extensionId.substring(point.length() + 1);
        return null;
    }

    private static List<String> versions(Map<String, String> values, String parentBranch) throws IOException {
        String configured = values.get("versions");
        if (configured == null || configured.trim().isEmpty()) configured = values.get("version");
        if (configured == null || configured.trim().isEmpty()) configured = parentBranch;
        if (configured == null || configured.trim().isEmpty()) throw new IOException("Flatpak extension point has no version");
        List<String> result = new ArrayList<>();
        for (String value : configured.split(";", -1)) {
            String version = value.trim();
            if (version.isEmpty() || !version.matches("[A-Za-z0-9_.+-]+"))
                throw new IOException("Invalid Flatpak extension point version");
            if (!result.contains(version)) result.add(version);
        }
        return result;
    }

    private static String branch(String ref) throws IOException {
        String[] parts = ref == null ? new String[0] : ref.split("/", -1);
        if (parts.length != 4) throw new IOException("Invalid Flatpak extension ref");
        return parts[3];
    }

    private static Map<String, Map<String, String>> parse(String metadata) throws IOException {
        Map<String, Map<String, String>> result = new LinkedHashMap<>();
        Map<String, String> current = null;
        for (String line : metadata.split("\\r?\\n", -1)) {
            String value = line.trim();
            if (value.isEmpty() || value.startsWith("#") || value.startsWith(";")) continue;
            if (value.startsWith("[")) {
                if (!value.endsWith("]") || value.length() < 3) throw new IOException("Invalid Flatpak metadata section");
                current = result.computeIfAbsent(value.substring(1, value.length() - 1), ignored -> new LinkedHashMap<>());
                continue;
            }
            int equals = value.indexOf('=');
            if (current != null && equals > 0) {
                String key = value.substring(0, equals).trim();
                if (current.putIfAbsent(key, value.substring(equals + 1).trim()) != null)
                    throw new IOException("Duplicate Flatpak metadata key " + key);
            }
        }
        return result;
    }
}
