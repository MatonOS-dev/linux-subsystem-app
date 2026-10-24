#!/usr/bin/env python3
"""Guard the boot cleanup path against background service starts."""
import pathlib
import re
import xml.etree.ElementTree as ET

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "linux/subsystem/host/app/src/main/java/org/matonos/linux/LinuxAppStoreService.java").read_text()
manifest = ET.parse(root / "linux/subsystem/host/app/src/main/AndroidManifest.xml").getroot()
android = "{http://schemas.android.com/apk/res/android}"

receiver = re.search(r"class BootReceiver extends .*?\n    }", source, re.S)
assert receiver, "BootReceiver is missing"
assert "JobScheduler" in receiver.group(0) and ".schedule(job)" in receiver.group(0), \
    "BootReceiver must enqueue cleanup with JobScheduler"
assert "startService(" not in receiver.group(0) and "startForegroundService(" not in receiver.group(0), \
    "BootReceiver must not start a service from the background"

service = next((item for item in manifest.findall("./application/service")
                if item.get(android + "name") == ".LinuxAppStoreService$CleanupJobService"), None)
assert service is not None, "Cleanup JobService is missing from the manifest"
assert service.get(android + "permission") == "android.permission.BIND_JOB_SERVICE", \
    "Cleanup JobService must require BIND_JOB_SERVICE"
assert "extends android.app.job.JobService" in source and "jobFinished(params,false)" in source, \
    "Cleanup JobService must finish scheduled work"
print("PASS boot cleanup is scheduled through the declared JobService")
