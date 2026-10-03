#ifndef MATON_FLATPAK_COMPAT_H
#define MATON_FLATPAK_COMPAT_H
#include <limits.h>
/* Deliberate broker compatibility floor: the image's current Flatpak release.
 * Future Flatpak incompatibilities require reviewing/updating this broker. */
#define MATON_BROKER_MIN_FLATPAK_VERSION "1.14.10"
#ifndef MATON_BROKER_BUILT_FLATPAK_VERSION
#define MATON_BROKER_BUILT_FLATPAK_VERSION "unknown (host build)"
#endif
/* Compare numeric release components, never strings (1.14.9 < 1.14.10).
 * Unknown/malformed releases are rejected rather than guessed compatible. */
static inline int maton_flatpak_version_parse(const char *text, unsigned version[3]) {
    for (int i=0;i<3;i++) {
        if (*text<'0' || *text>'9') return 0;
        unsigned value=0;
        do {
            unsigned digit=(unsigned)(*text++-'0');
            if (value>(UINT_MAX-digit)/10) return 0;
            value=value*10+digit;
        } while (*text>='0' && *text<='9');
        version[i]=value;
        if (i<2 && *text++!='.') return 0;
    }
    return *text==0;
}
static inline int maton_flatpak_supported(const char *system_version) {
    unsigned system[3], minimum[3];
    if (!maton_flatpak_version_parse(system_version,system) ||
        !maton_flatpak_version_parse(MATON_BROKER_MIN_FLATPAK_VERSION,minimum)) return 0;
    for (int i=0;i<3;i++) {
        if (system[i]!=minimum[i]) return system[i]>minimum[i];
    }
    return 1;
}
#endif
