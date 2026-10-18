#!/usr/bin/env python3
import subprocess
import sys
import zipfile

bootstrap, real, aapt2 = sys.argv[1:]

def badging(path):
    return subprocess.check_output([aapt2, 'dump', 'badging', path], text=True)

boot = badging(bootstrap)
host = badging(real)
assert "package: name='org.matonos.linux' versionCode='1'" in boot, boot.splitlines()[0]
assert "package: name='org.matonos.linux'" in host

def version(text):
    for line in text.splitlines():
        if line.startswith('package: '):
            return int(line.split("versionCode='")[1].split("'")[0])
    raise AssertionError('package metadata missing')

assert version(host) > version(boot), (version(boot), version(host))
with zipfile.ZipFile(bootstrap) as archive:
    info = archive.getinfo('assets/MatonLinuxSubsystem.apk')
    assert info.compress_type == zipfile.ZIP_STORED, 'embedded subsystem APK must be stored'
    assert archive.read(info) == open(real, 'rb').read(), 'bootstrap embeds a different subsystem APK'
print('bootstrap package/version and embedded APK verified')
