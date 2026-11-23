#!/usr/bin/env python3
"""Host-test local bundle staging without writing to the user's Flatpak install."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def run(args, *, env=None, check=True, capture=False):
    return subprocess.run(args, env=env, check=check, text=True,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} HELPER OUTPUT_DIR")
    helper = pathlib.Path(sys.argv[1]).resolve()
    out = pathlib.Path(sys.argv[2]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    out = pathlib.Path(tempfile.mkdtemp(prefix="stage-bundle-", dir=out))
    for tool in ("flatpak", "ostree"):
        if shutil.which(tool) is None:
            raise SystemExit(f"missing required host tool: {tool}")

    data_home = pathlib.Path(os.environ.get(
        "XDG_DATA_HOME", str(pathlib.Path.home() / ".local/share")))
    user_dir = pathlib.Path(os.environ.get("FLATPAK_USER_DIR", data_home / "flatpak"))
    source_repo = user_dir / "repo"
    if not source_repo.is_dir():
        raise SystemExit(f"no read-only source Flatpak repo: {source_repo}")

    refs = run(["flatpak", "list", "--user", "--runtime", "--columns=ref"],
               capture=True).stdout.splitlines()
    runtime = sdk = branch = None
    for ref in refs:
        parts = ref.split("/")
        if len(parts) == 3 and parts[0] == "org.freedesktop.Platform" and parts[1] == "x86_64":
            candidate = parts[2]
            sdk_ref = f"org.freedesktop.Sdk/x86_64/{candidate}"
            if sdk_ref in refs:
                runtime, sdk, branch = parts[0], "org.freedesktop.Sdk", candidate
                break
    if runtime is None:
        raise SystemExit("no locally installed matching org.freedesktop.Platform and org.freedesktop.Sdk")

    runtime_ref = f"runtime/{runtime}/x86_64/{branch}"
    commit = run(["ostree", f"--repo={source_repo}", "rev-parse", f"deploy/{runtime_ref}"],
                 capture=True).stdout.strip()
    runtime_repo = out / "runtime-repo"
    runtime_repo.mkdir(exist_ok=True)
    run(["ostree", f"--repo={runtime_repo}", "init", "--mode=archive-z2"])
    run(["ostree", f"--repo={runtime_repo}", "pull-local", str(source_repo), commit])
    run(["ostree", f"--repo={runtime_repo}", "refs", "--create=" + runtime_ref, commit])
    run(["ostree", f"--repo={runtime_repo}", "summary", "--update"])

    app_dir = out / "app-build"
    app_repo = out / "app-repo"
    run(["flatpak", "build-init", "--arch=x86_64", str(app_dir),
         "org.example.FlatpakBundleTest", f"{sdk}//{branch}", f"{runtime}//{branch}", "stable"])
    app_bin = app_dir / "files/app/bin/hello"
    app_bin.parent.mkdir(parents=True, exist_ok=True)
    app_bin.write_text("#!/bin/sh\necho bundle-test\n")
    app_bin.chmod(0o755)
    run(["flatpak", "build-finish", "--command=hello", str(app_dir)])
    run(["flatpak", "build-export", str(app_repo), str(app_dir), "stable"])
    run(["flatpak", "build-update-repo", str(app_repo)])
    bundle = out / "unsigned-app.flatpak"
    run(["flatpak", "build-bundle", str(app_repo), str(bundle),
         "org.example.FlatpakBundleTest", "stable"])

    for case in ("refused", "allowed"):
        case_data = out / case / "data"
        case_data.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env["XDG_DATA_HOME"] = str(case_data)
        run(["flatpak", "--user", "remote-add", "--no-gpg-verify", "test-runtime",
             runtime_repo.as_uri()], env=env)
        install = case_data / "flatpak"
        allow_unsigned = "false" if case == "refused" else "true"
        result = run([str(helper), "--stage-bundle", str(install), str(bundle), allow_unsigned],
                     env=env, check=False, capture=True)
        output = result.stdout
        errors = result.stderr
        if case == "refused":
            if result.returncode == 0 or "unsigned bundle refused" not in errors:
                raise SystemExit(f"unsigned refusal mismatch: rc={result.returncode}\n{errors}\n{output}")
            print(f"false: exit={result.returncode}; {errors.strip()}")
        else:
            expected_bundle = "BUNDLE\tUNSIGNED\tapp/org.example.FlatpakBundleTest/x86_64/stable"
            if result.returncode != 0 or expected_bundle not in output:
                raise SystemExit(f"unsigned allow mismatch: rc={result.returncode}\n{errors}\n{output}")
            if f"{runtime_ref}\ttest-runtime\t" not in output:
                raise SystemExit(f"runtime row missing from output:\n{output}")
            if "app/org.example.FlatpakBundleTest/x86_64/stable\t" not in output:
                raise SystemExit(f"app ref row missing from output:\n{output}")
            print(output, end="")

    if shutil.which("gpg") is not None:
        gnupg_home = out / "gnupg"
        gnupg_home.mkdir(mode=0o700)
        gpg_env = os.environ.copy()
        gpg_env["GNUPGHOME"] = str(gnupg_home)
        run(["gpg", "--batch", "--passphrase", "", "--quick-generate-key",
             "MatonOS bundle test <bundle-test@matonos.invalid>", "ed25519", "sign", "0"],
            env=gpg_env)
        key_listing = run(["gpg", "--batch", "--with-colons", "--list-secret-keys"],
                          env=gpg_env, capture=True).stdout
        key_id = next(line.split(":")[4] for line in key_listing.splitlines()
                      if line.startswith("sec:"))
        signed_repo = out / "signed-repo"
        run(["flatpak", "build-export", f"--gpg-sign={key_id}",
             f"--gpg-homedir={gnupg_home}", str(signed_repo), str(app_dir), "stable"])
        run(["flatpak", "build-update-repo", str(signed_repo)])
        signed_bundle = out / "signed-app.flatpak"
        run(["flatpak", "build-bundle", str(signed_repo), str(signed_bundle),
             "org.example.FlatpakBundleTest", "stable"])
        signed_data = out / "signed/data"
        signed_data.mkdir(parents=True, exist_ok=True)
        signed_env = os.environ.copy()
        signed_env["XDG_DATA_HOME"] = str(signed_data)
        run(["flatpak", "--user", "remote-add", "--no-gpg-verify", "test-runtime",
             runtime_repo.as_uri()], env=signed_env)
        result = run([str(helper), "--stage-bundle", str(signed_data / "flatpak"),
                      str(signed_bundle), "false"], env=signed_env,
                     check=False, capture=True)
        expected_signed = "BUNDLE\tSIGNED\tapp/org.example.FlatpakBundleTest/x86_64/stable"
        if result.returncode != 0 or expected_signed not in result.stdout:
            raise SystemExit(f"signed bundle mismatch: rc={result.returncode}\n{result.stderr}\n{result.stdout}")
        print(result.stdout, end="")
    print("stage-bundle host test: PASS")


if __name__ == "__main__":
    main()
