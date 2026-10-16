#!/usr/bin/env python3
"""Fetch verified compositor archives declared in UPSTREAMS; including the X11 client libraries."""
import hashlib, os, pathlib, re, tarfile, urllib.request
root = pathlib.Path(__file__).resolve().parents[1]
stage = pathlib.Path(os.environ['MATON_COMPOSITOR_BUILD'])
sources = pathlib.Path(os.environ.get('MATON_COMPOSITOR_SOURCES', str(stage / 'sources')))
text = (root / 'linux/subsystem/UPSTREAMS').read_text().split('minigbm (')[0]
for block in text.split('\n\n'):
    url = re.search(r'source: (https://\S+)', block)
    digest = re.search(r'(?:archive )?SHA-256: ([0-9a-f]{64})', block)
    if not url or not digest: continue
    name, version, *_ = block.splitlines()[0].split()
    if name == 'libxkbcommon': name = 'xkbcommon'
    name += '-' + version
    archive = sources / 'downloads' / (name + '.tar.gz')
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists(): urllib.request.urlretrieve(url[1], archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest[1]: raise SystemExit('Checksum mismatch: ' + str(archive))
    dest = sources / 'src' / name
    if dest.exists(): continue
    dest.mkdir(parents=True)
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            member.name = '/'.join(member.name.split('/')[1:])
            if member.name: tar.extract(member, dest, filter='data')

# X11 client inputs only; the image owns the Xwayland server.
for name, url, digest in [('xorgproto-2025.1', 'https://xorg.freedesktop.org/archive/individual/proto/xorgproto-2025.1.tar.xz', '56898c716c0578df8a2d828c9c3e5c528277705c0484381a81960fe1a67668e8'), ('xtrans-1.6.0', 'https://xorg.freedesktop.org/archive/individual/lib/xtrans-1.6.0.tar.xz', 'faafea166bf2451a173d9d593352940ec6404145c5d1da5c213423ce4d359e92'), ('util-macros-1.20.2', 'https://xorg.freedesktop.org/archive/individual/util/util-macros-1.20.2.tar.xz', '9ac269eba24f672d7d7b3574e4be5f333d13f04a7712303b1821b2a51ac82e8e'), ('libpthread-stubs-0.5', 'https://xorg.freedesktop.org/archive/individual/lib/libpthread-stubs-0.5.tar.xz', '59da566decceba7c2a7970a4a03b48d9905f1262ff94410a649224e33d2442bc'), ('xcb-proto-1.17.0', 'https://xcb.freedesktop.org/dist/xcb-proto-1.17.0.tar.xz', '2c1bacd2110f4799f74de6ebb714b94cf6f80fb112316b1219480fd22562148c'), ('libXau-1.0.12', 'https://xorg.freedesktop.org/archive/individual/lib/libXau-1.0.12.tar.xz', '74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb'), ('libXdmcp-1.1.5', 'https://xorg.freedesktop.org/archive/individual/lib/libXdmcp-1.1.5.tar.xz', 'd8a5222828c3adab70adf69a5583f1d32eb5ece04304f7f8392b6a353aa2228c'), ('libxcb-1.17.0', 'https://xcb.freedesktop.org/dist/libxcb-1.17.0.tar.xz', '599ebf9996710fea71622e6e184f3a8ad5b43d0e5fa8c4e407123c88a59a6d55'), ('xcb-util-wm-0.4.2', 'https://xcb.freedesktop.org/dist/xcb-util-wm-0.4.2.tar.xz', '62c34e21d06264687faea7edbf63632c9f04d55e72114aa4a57bb95e4f888a0b'), ('xcb-util-errors-1.0.1', 'https://xcb.freedesktop.org/dist/xcb-util-errors-1.0.1.tar.xz', '5628c87b984259ad927bacd8a42958319c36bdf4b065887803c9d820fb80f357')]:
    archive = sources / 'downloads' / (name + '.tar.xz')
    if not archive.exists(): urllib.request.urlretrieve(url, archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest: raise SystemExit('Checksum mismatch: ' + str(archive))
