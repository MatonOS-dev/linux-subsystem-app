# flatpak-spike: fixed NDK prebuilts until the Soong port is ready.
# The CLI launches this sandbox helper; a staged binary alone is not included
# in images unless the module is selected. matonos-bwrap is a Soong-built
# shim (Android.bp) that flatpak-env-wrapper selects as FLATPAK_BWRAP when
# the X11 relay socket exists.
PRODUCT_PACKAGES += bwrap
PRODUCT_PACKAGES += matonos-bwrap

FLATPAK_SYSTEM_EXT_PREBUILT := device/maton/pc_x86_64/linux/flatpak/prebuilt/system_ext
FLATPAK_SYSTEM_EXT_FILES := \
    $(wildcard $(FLATPAK_SYSTEM_EXT_PREBUILT)/bin/*) \
    $(wildcard $(FLATPAK_SYSTEM_EXT_PREBUILT)/lib64/*.so) \
    $(wildcard $(FLATPAK_SYSTEM_EXT_PREBUILT)/share/flatpak/triggers/*)

PRODUCT_COPY_FILES += $(foreach f,$(FLATPAK_SYSTEM_EXT_FILES),$(f):$(TARGET_COPY_OUT_SYSTEM_EXT)/$(patsubst $(FLATPAK_SYSTEM_EXT_PREBUILT)/%,%,$(f)))
