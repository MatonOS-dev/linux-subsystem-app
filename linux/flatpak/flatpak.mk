# flatpak-spike: fixed NDK prebuilts until the Soong port is ready.
# The CLI launches this sandbox helper; a staged binary alone is not included
# in images unless the module is selected. matonos-bwrap is a Soong-built
# shim (Android.bp) that flatpak-env-wrapper selects as FLATPAK_BWRAP when
# the X11 relay socket exists.
PRODUCT_PACKAGES += bwrap libseccomp_matonos
PRODUCT_PACKAGES += matonos-bwrap
PRODUCT_PACKAGES += matonos-app-exec
PRODUCT_PACKAGES += matonos-mount-helper

FLATPAK_SYSTEM_EXT_PREBUILT := device/maton/pc_x86_64/linux/flatpak/prebuilt/system_ext
# Fail staging if the validated seccomp-enabled CLI is missing/stale.
FLATPAK_SECCOMP_VALID := $(shell grep -qx '\#define ENABLE_SECCOMP 1' device/maton/pc_x86_64/linux/flatpak/seccomp-config.h && cd $(FLATPAK_SYSTEM_EXT_PREBUILT) && sha256sum -c ../../seccomp.sha256 >/dev/null 2>&1 && echo yes)
ifneq ($(FLATPAK_SECCOMP_VALID),yes)
$(error Flatpak seccomp evidence missing or binary changed; run linux/flatpak/build-seccomp.sh)
endif
FLATPAK_SYSTEM_EXT_FILES := $(filter-out $(FLATPAK_SYSTEM_EXT_PREBUILT)/bin/matonos-dbus-broker,\
    $(wildcard $(FLATPAK_SYSTEM_EXT_PREBUILT)/bin/*) \
    $(wildcard $(FLATPAK_SYSTEM_EXT_PREBUILT)/lib64/*.so) \
    $(wildcard $(FLATPAK_SYSTEM_EXT_PREBUILT)/share/flatpak/triggers/*))

PRODUCT_COPY_FILES += $(foreach f,$(FLATPAK_SYSTEM_EXT_FILES),$(f):$(TARGET_COPY_OUT_SYSTEM_EXT)/$(patsubst $(FLATPAK_SYSTEM_EXT_PREBUILT)/%,%,$(f)))
