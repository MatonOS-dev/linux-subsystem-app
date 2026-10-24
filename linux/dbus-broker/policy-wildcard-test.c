/* "own *" / "talk *" on an app's private bus: any name except the reserved
 * portal, bus and host-service names. */
#include "broker.c"
#include <assert.h>

int main(void) {
    char directory[] = "/tmp/maton-policy-XXXXXX"; assert(mkdtemp(directory));
    char policy[128], bus[128];
    snprintf(policy, sizeof(policy), "%s/policy", directory);
    snprintf(bus, sizeof(bus), "%s/bus", directory);
    assert(g_file_set_contents(policy, "own org.example.App\nown *\nown org.freedesktop.portal.Flatpak\ntalk *\ntalk org.freedesktop.portal.Desktop\n", -1, NULL));
    Broker *b = broker_new(bus, policy, NULL); assert(b);
    assert(access_for(b, "org.example.App") == (ACCESS_OWN | ACCESS_TALK));
    assert(access_for(b, "com.steampowered.PressureVessel.LaunchAlongsideSteam") == (ACCESS_OWN | ACCESS_TALK));
    /* Reserved names never come from the wildcard; explicit rules still apply. */
    assert(access_for(b, "org.freedesktop.portal.Desktop") == ACCESS_TALK);
    assert(access_for(b, "org.freedesktop.portal.Flatpak") == ACCESS_OWN);
    assert(access_for(b, "org.freedesktop.impl.portal.Settings") == 0);
    assert(access_for(b, "org.freedesktop.DBus") == 0);
    assert(access_for(b, "org.freedesktop.Flatpak") == 0);
    assert(access_for(b, "org.freedesktop.systemd1") == 0);
    broker_free(b); unlink(policy); rmdir(directory);
    puts("PASS: explicit Flatpak portal ownership with other reserved names excluded");
    return 0;
}
