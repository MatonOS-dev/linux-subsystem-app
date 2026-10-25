package org.matonos.linux.stub;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;

/** Maps Android's ordered BCP-47 locale list to Flatpak Locale extension languages. */
public final class FlatpakLanguages {
    private FlatpakLanguages() {}

    public static List<String> fromLanguageTags(List<String> tags) {
        LinkedHashSet<String> languages = new LinkedHashSet<>();
        if (tags != null) for (String tag : tags) {
            if (tag == null || tag.trim().isEmpty()) continue;
            String language = Locale.forLanguageTag(tag.trim()).getLanguage();
            // `flatpak config languages` accepts two-letter ISO language codes.
            if (language.matches("[A-Za-z]{2}")) languages.add(language.toLowerCase(Locale.ROOT));
        }
        // An empty Flatpak language setting means "all languages", so retain
        // a small safe fallback if Android supplies no representable locale.
        if (languages.isEmpty()) languages.add("en");
        return new ArrayList<>(languages);
    }

    public static String configuration(List<String> languageTags) {
        return String.join(";", fromLanguageTags(languageTags));
    }

    public static boolean changed(String previous, String current) {
        return previous == null || !previous.equals(current);
    }

    public static boolean isLocaleRef(String ref) {
        if (ref == null) return false;
        String[] fields = ref.split("/", -1);
        return fields.length == 4 && "runtime".equals(fields[0]) && fields[1].endsWith(".Locale")
                && !fields[2].isEmpty() && !fields[3].isEmpty();
    }

    public static List<String> refsToUpdate(String previous, String current, List<String> installedRefs) {
        if (!changed(previous, current) || installedRefs == null) return new ArrayList<>();
        LinkedHashSet<String> refs = new LinkedHashSet<>();
        for (String ref : installedRefs) if (isLocaleRef(ref)) refs.add(ref);
        return new ArrayList<>(refs);
    }
}
