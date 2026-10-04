# r24: MatonOS's Flatpak stack ships as one updatable APEX,
# com.matonos.flatpak (linux/flatpak/Android.bp), preinstalled on system_ext at
# /system_ext/apex/com.matonos.flatpak.apex and mounted at
# /apex/com.matonos.flatpak. The Flatpak CLI/portal/libs, bwrap,
# libseccomp_matonos, matonos-bwrap, matonos-app-exec, matonos-mount-helper,
# matonos-flatpak-store, xdg-dbus-proxy, ostree and the Flatpak triggers used
# to be PRODUCT_COPY_FILES / PRODUCT_PACKAGES into /system_ext; they are APEX
# contents now. The D-Bus broker stays in MatonWaylandHost.apk; linuxd stays in
# system_ext and execs paths under the APEX.
PRODUCT_PACKAGES += com.matonos.flatpak

# Fail staging if the validated seccomp-enabled CLI is missing/stale. The
# APEX's cc_prebuilt_binary modules copy exactly these prebuilt files, so the
# same evidence gate still applies (see linux/flatpak/APEX.md).
FLATPAK_SYSTEM_EXT_PREBUILT := device/maton/pc_x86_64/linux/flatpak/prebuilt/system_ext
FLATPAK_SECCOMP_VALID := $(shell grep -qx '\#define ENABLE_SECCOMP 1' device/maton/pc_x86_64/linux/flatpak/seccomp-config.h && cd $(FLATPAK_SYSTEM_EXT_PREBUILT) && sha256sum -c ../../seccomp.sha256 >/dev/null 2>&1 && echo yes)
ifneq ($(FLATPAK_SECCOMP_VALID),yes)
$(error Flatpak seccomp evidence missing or binary changed; run linux/flatpak/build-seccomp-apex.sh (or build-seccomp.sh))
endif
