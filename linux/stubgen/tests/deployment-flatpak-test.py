#!/usr/bin/env python3
"""Exercise the package-derived lookup tree with stock host Flatpak/OSTree.
Requires python3-gi (GLib), flatpak, ostree and the compiled stubgen tests.
Usage: deployment-flatpak-test.py CLASSES OUTPUT_DIRECTORY
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from gi.repository import GLib

classes = str(Path(sys.argv[1]).resolve())
output = Path(sys.argv[2]).resolve()
output.mkdir(parents=True, exist_ok=True)

def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()

with tempfile.TemporaryDirectory(prefix='flatpak-lookup-', dir=output) as temp:
    root = Path(temp)
    repo, source = root / 'repo', root / 'source'
    source.mkdir()
    (source / 'metadata').write_text('[Application]\nname=org.test.App\nruntime=org.test.Platform/x86_64/stable\ncommand=test\n')
    run('ostree', 'init', '--repo=' + str(repo), '--mode=bare-user')
    commit = run('ostree', 'commit', '--repo=' + str(repo), '--branch=test',
                 '--tree=dir=' + str(source), '--subject=lookup-test')
    deploy = root / 'deploy'
    deploy.write_bytes(GLib.Variant('(ssasta{sv})', ('flathub', commit, [], 0, {})).get_data_as_bytes().get_data())
    probe = root / 'ScaffoldProbe.java'
    probe.write_text('''import org.matonos.linuxhost.stubgen.DeploymentRecord;
import java.io.*;import java.nio.file.*;
public class ScaffoldProbe {
 public static void main(String[] a)throws Exception {
  new DeploymentRecord("app/org.test.App/x86_64/stable",a[2],"flathub",
   Files.readAllBytes(Path.of(a[4])),Files.readAllBytes(Path.of(a[1])),Files.readAllBytes(Path.of(a[3])))
   .materialize(new File(a[0]),"/app");
 }
}''')
    run('javac', '-cp', classes, '-d', str(root), str(probe))
    installation = root / 'installation'
    run('java', '-cp', str(root) + ':' + classes, 'ScaffoldProbe', str(installation), str(deploy), commit,
        str(repo / 'objects' / commit[:2] / (commit[2:] + '.commit')), str(source / 'metadata'))
    env = dict(os.environ, FLATPAK_USER_DIR=str(installation))
    for flag, expected in [('--show-commit', commit), ('--show-origin', 'flathub')]:
        value = run('flatpak', 'info', '--user', flag, 'app/org.test.App/x86_64/stable', env=env)
        assert value == expected, (flag, value)
    # The lookup tree has metadata only: no shared store or copied app payload.
    assert (installation / 'app/org.test.App/x86_64/stable' / commit / 'files').is_symlink()
    print('PASS stock Flatpak reads commit and origin from package-derived deployment metadata')
