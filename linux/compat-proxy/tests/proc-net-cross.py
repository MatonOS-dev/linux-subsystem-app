#!/usr/bin/env python3
"""TCP inode discovery in both directions across separate i386/x86_64 proxies."""
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile

source = Path(__file__).resolve().parent.parent
out = Path(os.environ["COMPAT_LINKS_TEST_OUT"]).resolve()
out.mkdir(parents=True, exist_ok=True)
ndk = Path(os.environ.get("MATON_ANDROID_NDK", os.environ.get(
    "MATON_NDK", str(Path.home() / "Documents/android-ndk-r30"))))
compiler = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android36-clang"
libc = os.environ.get("MATON_V3_I386_LIBC", "/usr/lib32/libc.so.6")
interpreter = os.environ.get("MATON_V3_I386_INTERPRETER", "/lib/ld-linux.so.2")
fixture = source / "tests/proc-net-peer.c"
subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-no-pie",
                str(fixture), "-o", str(out / "peer64")], check=True)
subprocess.run([str(compiler), "-O2", "-fno-stack-protector", "-Wall", "-Wextra",
                "-Werror", "-c", str(fixture), "-o", str(out / "peer32.o")], check=True)
subprocess.run(["cc", "-m32", "-c", str(source / "tests/i386-entry.S"),
                "-o", str(out / "entry32.o")], check=True)
subprocess.run(["ld", "-m", "elf_i386", "--dynamic-linker", interpreter,
                str(out / "entry32.o"), str(out / "peer32.o"), libc,
                "-o", str(out / "peer32")], check=True)
loaders = {64: source / "build/linux-layer-v3", 32: source / "build/linux-layer-v3-i386"}
with tempfile.TemporaryDirectory(prefix="tcp-cross-", dir=out) as tmp:
    env = dict(os.environ, TMPDIR=tmp)
    for producer_abi, reader_abi in [(32, 64), (64, 32)]:
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(10)
            port = listener.getsockname()[1]
            with (out / f"producer-{producer_abi}.log").open("w") as log:
                producer = subprocess.Popen([str(loaders[producer_abi]), "--android-filter",
                    str(out / f"peer{producer_abi}"), str(port)], env=env,
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
                try:
                    if not select.select([producer.stdout], [], [], 10)[0]:
                        raise AssertionError("producer did not connect")
                    fd = int(producer.stdout.readline())
                    with listener.accept()[0]:
                        inode = os.stat(f"/proc/{producer.pid}/fd/{fd}").st_ino
                        subprocess.run([str(loaders[reader_abi]), "--android-filter",
                            str(out / f"peer{reader_abi}"), str(port), str(inode)],
                            env=env, check=True, timeout=10, stderr=log)
                finally:
                    producer.communicate(b"x", timeout=10)
                if producer.returncode:
                    raise AssertionError(f"producer exited {producer.returncode}")
            print(f"PASS {producer_abi}-bit TCP socket inode visible to {reader_abi}-bit peer")
