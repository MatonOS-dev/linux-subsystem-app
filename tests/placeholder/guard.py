#!/usr/bin/env python3
"""Guard: staging-content reads must go through PlaceholderFiles.

The v3.5 compatibility proxy represents emulated hardlinks as indexed regular
placeholders (an empty S_ISVTX file plus a `.v3-inodes/p-<dev>-<ino>` entry
naming the backing object). Java code that reads a staged/operation tree
natively sees the empty placeholder, so every such read must be resolved with
`org.matonos.linuxhost.stubgen.PlaceholderFiles`.

This static guard scans the subsystem and stubgen packages for raw
content-reading constructs. A match is allowed only when the same line routes
through `PlaceholderFiles`, or when the exact (file, pattern) pair and count is
listed below with a reason explaining why the path is app/APK-owned and not a
staged tree. Adding a new raw read of staging content fails the guard.

Usage: python3 tests/placeholder/guard.py [REPO_ROOT]
"""
import os
import re
import sys

PATTERNS = {
    "files-readAllBytes": re.compile(r"Files\.readAllBytes\("),
    "files-newInputStream": re.compile(r"Files\.newInputStream\("),
    "files-newByteChannel": re.compile(r"Files\.newByteChannel\("),
    "files-readString": re.compile(r"Files\.readString\("),
    "files-lines": re.compile(r"Files\.lines\("),
    "files-newBufferedReader": re.compile(r"Files\.newBufferedReader\("),
    "files-copy": re.compile(r"Files\.copy\("),
    "fileinputstream": re.compile(r"new (?:java\.io\.)?FileInputStream\("),
    "randomaccessfile": re.compile(r"new (?:java\.io\.)?RandomAccessFile\("),
    "filechannel-open": re.compile(r"FileChannel\.open\("),
    "zipfile": re.compile(r"new (?:java\.util\.zip\.)?ZipFile\("),
    "zipinputstream": re.compile(r"new (?:java\.util\.zip\.)?ZipInputStream\("),
}

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                       os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

SCAN = [
    "linux/subsystem/host/app/src/main/java/org/matonos/linux/stub",
    "linux/stubgen/src/org/matonos/linuxhost/stubgen",
]
SCAN_FILES = [
    "linux/subsystem/host/app/src/main/java/org/matonos/linux/LinuxAppStoreService.java",
    "linux/subsystem/host/app/src/main/java/org/matonos/linux/XwaylandFiles.java",
]

# (file, pattern) -> (allowed occurrences, reason). Every raw read must be
# either routed through PlaceholderFiles on the same line or justified here.
ALLOW = {
    ("StubGenerator.java", "fileinputstream"): (
        3, "hashes and streams the freshly built EROFS image (mkfs output in a temp dir) into its STORED split entry, not a .v3-store-root tree"),
    ("LinuxAppStoreService.java", "files-readAllBytes"): (
        1, "reads the objects.index writer artifact under flatpak-pack-artifacts, not a .v3-store-root tree"),
    ("LinuxAppStoreService.java", "files-copy"): (
        1, "copies an installed PackageManager split APK (app-owned), not staging"),
    ("XwaylandFiles.java", "files-copy"): (
        1, "extracts a host-APK asset into the runtime directory, not staging"),
    ("PackInstaller.java", "fileinputstream"): (
        1, "streams an already-built artifact into the PackageInstaller session, not staging"),
    ("ExtraService.java", "randomaccessfile"): (
        1, "reads an installed pack_ split carrier (app-owned), not staging"),
    ("ExtraService.java", "fileinputstream"): (
        1, "streams the freshly built extra APK over the result pipe, not staging"),
    ("ExtraService.java", "zipfile"): (
        1, "reads the installed index_ split APK (app-owned), not staging"),
    ("InstallTransaction.java", "files-copy"): (
        1, "copies the bundled remote descriptor asset, not staging"),
    ("InstallTransaction.java", "files-readAllBytes"): (
        1, "reads a File.createTempFile flatpak error log, not staging"),
    ("InstallTransaction.java", "zipfile"): (
        1, "reads the installed index_ split APK (app-owned), not staging"),
    ("StagedPacks.java", "files-readAllBytes"): (
        1, "reads a File.createTempFile ostree error log, not staging"),
    ("ObjectPacks.java", "fileinputstream"): (
        1, "digest() re-reads a pack just written to an app-owned output directory"),
    ("ObjectPacks.java", "files-readAllBytes"): (
        1, "readIndex() reads an objects.index/original.index writer artifact, not staging"),
    ("ObjectPacks.java", "randomaccessfile"): (
        2, "zip() and carrier() write/read app-owned output packs, not staging"),
    ("ObjectPacks.java", "files-copy"): (
        1, "reuses an app-owned installed carrier in update(), not staging"),
    ("OstreePacks.java", "zipinputstream"): (
        1, "reads original metadata supplied from an installed split APK, not staging"),
    ("DeploymentRecord.java", "zipfile"): (
        1, "reads the installed index_ split APK (app-owned), not staging"),
    ("PackLaunch.java", "zipfile"): (
        3, "reads installed index_/extra_ splits and the signed subsystem GL pack archive (app-owned), not staging"),
    ("PackageDeployment.java", "zipfile"): (
        1, "reads ref metadata and the commit object from the signed subsystem GL extension archive, not staging"),
    ("V3Launcher.java", "files-readAllBytes"): (
        1, "reads the adb-written <cache>/v3-command debug file in the stub's own cache, not staging"),
    ("PackSeed.java", "randomaccessfile"): (
        1, "reads an installed pack_ split carrier (app-owned), not staging"),
    ("PackSessionPlan.java", "zipfile"): (
        1, "compares two installed base APKs in sameBase(), not staging"),
    ("StubGenerator.java", "files-readAllBytes"): (
        5, "reads objects/index writer artifacts under the app-owned pack directory"),
    ("StubGenerator.java", "zipfile"): (
        1, "reads a generated/installed split APK, not staging"),
}

# Files that read staged content and must keep routing through the helper.
REQUIRED_HELPER_USERS = {
    "DeployManifest.java", "StagedPacks.java", "LauncherData.java",
    "InstallTransaction.java", "ObjectPacks.java", "OstreePacks.java",
}


def audited_files():
    result = []
    for relative in SCAN:
        base = os.path.join(ROOT, relative)
        for directory, _, names in os.walk(base):
            for name in sorted(names):
                if name.endswith(".java"):
                    result.append(os.path.join(directory, name))
    for relative in SCAN_FILES:
        result.append(os.path.join(ROOT, relative))
    return result


def main():
    counts = {}
    helper_users = set()
    for path in audited_files():
        name = os.path.basename(path)
        if name == "PlaceholderFiles.java":
            continue
        with open(path, encoding="utf-8") as handle:
            lines = handle.readlines()
        if any("PlaceholderFiles." in line for line in lines):
            helper_users.add(name)
        for line in lines:
            for label, pattern in PATTERNS.items():
                if not pattern.search(line):
                    continue
                if "PlaceholderFiles." in line:
                    continue
                counts[(name, label)] = counts.get((name, label), 0) + 1
    failures = []
    if os.environ.get("GUARD_DUMP"):
        for key in sorted(counts):
            print("DUMP {} {}".format(key, counts[key]))
        return 0
    for key in sorted(set(counts) | set(ALLOW)):
        seen = counts.get(key, 0)
        allowed, reason = ALLOW.get(key, (0, None))
        if seen > allowed:
            failures.append("{}: {} raw read(s) of pattern '{}' (allowed {}): {}".format(
                key[0], seen, key[1], allowed,
                reason or "NO allowlist entry - route it through PlaceholderFiles"))
        elif seen < allowed:
            failures.append("{}: allowlist permits {} '{}' but only {} found; update the allowlist and its reason"
                            .format(key[0], allowed, key[1], seen))
    for name in sorted(REQUIRED_HELPER_USERS):
        if name not in helper_users:
            failures.append("{}: no PlaceholderFiles routing found".format(name))
    if failures:
        print("PLACEHOLDER READ GUARD FAILED")
        for failure in failures:
            print("  " + failure)
        return 1
    print("PASS placeholder read guard ({} allowlisted raw sites)".format(len(ALLOW)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
