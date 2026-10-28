#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
fixture=$(mktemp -d "$PWD/build/home-links.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
mkdir "$fixture/home" "$fixture/classes"
chmod 700 "$fixture/home"
cat > "$fixture/HomeLinksTest.java" <<'JAVA'
package org.matonos.linux.stub;
import java.io.File;
import java.nio.file.Files;
public class HomeLinksTest {
    public static void main(String[] args) throws Exception {
        File home = new File(args[0]);
        HomeLinkStore.prepare(home);
        File marker = new File(home, ".v3-store-root");
        Object inode = Files.getAttribute(marker.toPath(), "unix:ino");
        HomeLinkStore.prepare(home);
        if (!inode.equals(Files.getAttribute(marker.toPath(), "unix:ino")))
            throw new AssertionError("launch replaced persistent link store");
    }
}
JAVA
javac -d "$fixture/classes" \
    ../subsystem/host/app/src/main/java/org/matonos/linux/stub/HomeLinkStore.java \
    "$fixture/HomeLinksTest.java"
cc -O2 -Wall -Wextra -Werror tests/home-links.c -o build/home-links-test
cc -O2 -Wall -Wextra -Werror tests/link-deny.c -o build/link-deny
# Same runtime operation fails before the launcher's marker is established.
mkdir "$fixture/unmarked"
if timeout 20 build/link-deny "$PWD/build/linux-layer-v3" --android-filter \
    "$PWD/build/home-links-test" "$fixture/unmarked" >"$fixture/before.out" 2>"$fixture/before.log"; then
    echo 'FAIL unmarked runtime unexpectedly linked with kernel linkat denied' >&2
    exit 1
fi
grep -q 'Permission denied' "$fixture/before.log"
java -cp "$fixture/classes" org.matonos.linux.stub.HomeLinksTest "$fixture/home"
timeout 20 build/link-deny "$PWD/build/linux-layer-v3" --android-filter \
    "$PWD/build/home-links-test" "$fixture/home" >"$fixture/out" 2>"$fixture/log"
cat "$fixture/out"
if command -v google-chrome >/dev/null; then
    mkdir -p "$fixture/chrome-home"
    chmod 700 "$fixture/chrome-home"
    java -cp "$fixture/classes" org.matonos.linux.stub.HomeLinksTest "$fixture/chrome-home"
    HOME="$fixture/chrome-home" \
        XDG_CONFIG_HOME="$fixture/chrome-home/.var/app/com.google.Chrome/config" \
        XDG_CACHE_HOME="$fixture/chrome-home/.cache" timeout 60 build/link-deny \
        "$PWD/build/linux-layer-v3" --android-filter "$(command -v google-chrome)" \
        --headless --no-sandbox --disable-gpu --disable-dev-shm-usage \
        --disable-background-networking \
        --user-data-dir="$fixture/chrome-home/.var/app/com.google.Chrome/config/google-chrome" \
        --dump-dom about:blank >"$fixture/chrome.out" 2>"$fixture/chrome.log"
    grep -q '<html>' "$fixture/chrome.out"
    printf 'PASS host Chrome starts with its profile and Singleton symlinks inside the HOME link store\n'
fi
