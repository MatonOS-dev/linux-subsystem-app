from pathlib import Path

root = Path(__file__).resolve().parents[1]
bootstrap = (root / 'linux/subsystem/host/bootstrap/build.gradle').read_text()
real = (root / 'linux/subsystem/host/app/build.gradle').read_text()
build = (root / 'build.sh').read_text()
assert 'versionCode 1' in bootstrap
assert "findProperty('realVersionCode')" in real and 'realVersionCode <= 1' in real
assert 'git -C "$ROOT" rev-list --count HEAD' in build
assert 'REAL_VERSION_CODE -gt 1' in build
print('bootstrap versionCode is 1; real versionCode is monotonic and greater than 1')
