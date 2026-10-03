/* Compatibility boundaries must compare Flatpak releases numerically and
 * accept newer system releases without requiring equality to the build. */
#include "flatpak-compat.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(!maton_flatpak_supported("1.14.9"));
    assert(!maton_flatpak_supported("1.13.99"));
    assert(!maton_flatpak_supported("0.99.99"));
    assert(maton_flatpak_supported("1.14.10"));
    assert(maton_flatpak_supported("1.14.11"));
    assert(maton_flatpak_supported("1.15.0"));
    assert(maton_flatpak_supported("1.100.0"));
    assert(maton_flatpak_supported("2.0.0"));
    const char* invalid[]={"", "unknown", "1.14", "1.14.10.0", "1.14.10junk",
        "-1.14.10", "1..10", "1.14.", "9999999999999999999999999.0.0"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)
        assert(!maton_flatpak_supported(invalid[i]));
    puts("PASS: minimum/equal/newer releases, numeric ordering and malformed/unknown versions");
    return 0;
}
