#!/usr/bin/env python3
"""Scan every reachable Git blob (including deleted files) without printing secrets."""
import json, re, subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parents[1]
def git(*args): return subprocess.check_output(['git', '-C', str(root), *args])
objects = git('rev-list', '--objects', '--all').splitlines()
paths = {}; findings = []
for line in objects:
    oid, _, name = line.partition(b' ')
    paths.setdefault(oid, name.decode(errors='replace'))
    if re.search(rb'\.(?:pem|pk8|jks|keystore|p12|pfx|key)$', name, re.I):
        findings.append({'object': oid.decode(), 'path': name.decode(errors='replace'), 'reason': 'signing/private-key filename'})
proc = subprocess.Popen(['git', '-C', str(root), 'cat-file', '--batch'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
blobs = 0
for oid, name in paths.items():
    proc.stdin.write(oid+b'\n'); proc.stdin.flush()
    header = proc.stdout.readline().split()
    data = proc.stdout.read(int(header[2])); proc.stdout.read(1)
    if header[1] != b'blob': continue
    blobs += 1
    # Match actual PEM blocks, not documentation mentioning the delimiters.
    if re.search(rb'-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----\s*\n[A-Za-z0-9+/=\r\n]{32,}-----END', data) or data.startswith(b'\xfe\xed\xfe\xed'):
        findings.append({'object': oid.decode(), 'path': name, 'reason': 'private key / JKS content'})
proc.stdin.close(); proc.wait()
print(json.dumps({'reachable_objects': len(paths), 'blobs_scanned': blobs, 'findings': findings}, indent=2))
sys.exit(bool(findings))
