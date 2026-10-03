#ifndef MATON_SESSION_CONTROL_H
#define MATON_SESSION_CONTROL_H
#include <sys/types.h>
#define MATON_SESSION_FLATPAK_UNSUPPORTED 4
#define MATON_SESSION_FLATPAK_ERROR "org.matonos.DBus.Error.UnsupportedFlatpakVersion"
/* Private native registration, not exposed inside Flatpak sandboxes.
 * SO_PEERCRED authenticates the supervisor; its child remains gated until
 * accepted and pidfd_open pins the child before its D-Bus RequestName.
 * Compatibility is the system Flatpak version only, with no protocol version.
 * Status: 0 = start child, 2 = reuse supervisor, 3 = unavailable,
 * 4 = unsupported/unknown system Flatpak (error/message carry a D-Bus error).
 * After status 0, byte 1 acknowledges the portal's RequestName.
 * Keep control connected until portal exit or session/broker shutdown.
 * Exact packet lengths are required; missing version metadata fails closed. */
struct MatonSessionRegistration { pid_t pid; char monitor[192]; char flatpak_version[64]; };
struct MatonSessionReply { int status; pid_t supervisor; char error[96]; char message[256]; };
#endif
