# Coordinator documentation change

Add to NOTES.md "Dropped for zero patches": non-restarting packed-app content
updates are unavailable on this stock checkout. PackageInstallerSession.java
4836–4858 clears INSTALL_DONT_KILL_APP whenever an existing split is replaced
or removed. A content-addressed index replacement must remove its predecessor.
Achieving the requested process lifetime would require changing stock PM or
changing the agreed index/removal protocol; neither was done. Keep the thin
adapter request and atomic split update, but do not promise PID preservation.

No shared build/SELinux/bridge/kernel source changes are requested. Build the
host APK with the coordinator's existing signing keys and regenerate the normal
bridge certificate allowlist using build-apps.sh; task-local generated allowlist
changes were restored instead of committing a workstation development key.
