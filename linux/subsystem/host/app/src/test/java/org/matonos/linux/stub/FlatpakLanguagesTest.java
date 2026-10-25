package org.matonos.linux.stub;

import static org.junit.Assert.*;
import java.util.Arrays;
import java.util.Collections;
import org.junit.Test;

public class FlatpakLanguagesTest {
    @Test public void mapsOrderedAndroidLocalesToFlatpakLanguageCodes() {
        assertEquals("de;fr",FlatpakLanguages.configuration(Arrays.asList("de-DE","fr-CA","de-AT")));
        assertEquals("zh",FlatpakLanguages.configuration(Collections.singletonList("zh-Hans-CN")));
        assertEquals("en;de",FlatpakLanguages.configuration(Arrays.asList("en-US","de-DE")));
        assertEquals("en",FlatpakLanguages.configuration(Collections.singletonList("fil-PH")));
    }

    @Test public void localeUpdateQueuesOnlyInstalledLocaleRefsWhenTheSetChanges() {
        String locale="runtime/org.freedesktop.Platform.Locale/x86_64/26.08";
        String runtime="runtime/org.freedesktop.Platform/x86_64/26.08";
        assertEquals(Collections.singletonList(locale),FlatpakLanguages.refsToUpdate("en", "de;en",
                Arrays.asList(runtime,locale,locale)));
        assertTrue(FlatpakLanguages.refsToUpdate("de;en", "de;en",Collections.singletonList(locale)).isEmpty());
        assertTrue(FlatpakLanguages.refsToUpdate(null, "en",Collections.singletonList(runtime)).isEmpty());
    }
}
