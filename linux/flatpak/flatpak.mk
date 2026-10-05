# r24: MatonOS's Flatpak stack ships as one updatable APEX,
# com.matonos.flatpak (linux/flatpak/Android.bp), preinstalled on system_ext at
# /system_ext/apex/com.matonos.flatpak.apex and mounted at
# /apex/com.matonos.flatpak. The static multicall Flatpak/OSTree/bwrap binary,
# static-musl launch helpers and namespace launcher, store helper, and
# Flathub trust/config files ship in the APEX. linuxd and the D-Bus broker stay
# outside it. See APEX.md for the member-by-member integration plan.
PRODUCT_PACKAGES += com.matonos.flatpak
