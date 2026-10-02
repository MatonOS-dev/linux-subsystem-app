# APK signing dependency

`apksig.jar` contains unmodified upstream `tools/apksig/src/main/java` classes
from this AOSP checkout (excluding cloud KMS providers). It is copied from the
existing SDK Java compilation output:

`out/soong/.intermediates/tools/apksig/apksig/android_common/javac/apksig.jar`

AOSP's `apksig` Soong module declares only the `com.android.webapp` APEX as an
allowed deployment target. The device-owned `MatonApkSig` import makes the same
classes available to the platform bridge without changing that upstream module.
To refresh, build the upstream apksig target through the coordinating build,
then copy its javac jar here. The code is Apache 2.0 licensed; see LICENSE.

Upstream checkout commit: `179f60df00d242f6bb22acf828b0884eac2d5f72`.
Jar SHA-256: `a12add55448e5298dcb0ebb7b65f29d302030896d17892316a5663c0e60d0542`.
