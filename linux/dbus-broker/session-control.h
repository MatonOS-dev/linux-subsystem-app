#ifndef MATON_SESSION_CONTROL_H
#define MATON_SESSION_CONTROL_H
#include <sys/types.h>
/* Local native protocol, not exposed inside Flatpak sandboxes.
 * The supervisor's connected peer PID is authenticated by SO_PEERCRED.
 * Its child is blocked on a pipe until registration succeeds. pidfd_open
 * pins that child; RequestName must match its authenticated live PID.
 * Reply status: 0 = start gated child, 2 = reuse supervisor, 3 = unavailable.
 * After status 0, a single byte 1 acknowledges the portal's RequestName.
 * Keep control connected until portal exit or session/broker shutdown. */
struct MatonSessionRegistration { pid_t pid; char monitor[192]; };
struct MatonSessionReply { int status; pid_t supervisor; };
#endif
