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

# X11 client libraries, then the APK-packaged Xwayland server and its inputs.
for name, url, digest in [('libgpg-error-1.51', 'https://gnupg.org/ftp/gcrypt/libgpg-error/libgpg-error-1.51.tar.bz2', 'be0f1b2db6b93eed55369cdf79f19f72750c8c7c39fc20b577e724545427e6b2'), ('libgcrypt-1.11.1', 'https://gnupg.org/ftp/gcrypt/libgcrypt/libgcrypt-1.11.1.tar.bz2', '24e91c9123a46c54e8371f3a3a2502f1198f2893fbfbf59af95bc1c21499b00e'), ('xorgproto-2025.1', 'https://xorg.freedesktop.org/archive/individual/proto/xorgproto-2025.1.tar.xz', '56898c716c0578df8a2d828c9c3e5c528277705c0484381a81960fe1a67668e8'), ('xtrans-1.6.0', 'https://xorg.freedesktop.org/archive/individual/lib/xtrans-1.6.0.tar.xz', 'faafea166bf2451a173d9d593352940ec6404145c5d1da5c213423ce4d359e92'), ('util-macros-1.20.2', 'https://xorg.freedesktop.org/archive/individual/util/util-macros-1.20.2.tar.xz', '9ac269eba24f672d7d7b3574e4be5f333d13f04a7712303b1821b2a51ac82e8e'), ('libpthread-stubs-0.5', 'https://xorg.freedesktop.org/archive/individual/lib/libpthread-stubs-0.5.tar.xz', '59da566decceba7c2a7970a4a03b48d9905f1262ff94410a649224e33d2442bc'), ('xcb-proto-1.17.0', 'https://xcb.freedesktop.org/dist/xcb-proto-1.17.0.tar.xz', '2c1bacd2110f4799f74de6ebb714b94cf6f80fb112316b1219480fd22562148c'), ('libXau-1.0.12', 'https://xorg.freedesktop.org/archive/individual/lib/libXau-1.0.12.tar.xz', '74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb'), ('libXdmcp-1.1.5', 'https://xorg.freedesktop.org/archive/individual/lib/libXdmcp-1.1.5.tar.xz', 'd8a5222828c3adab70adf69a5583f1d32eb5ece04304f7f8392b6a353aa2228c'), ('libxcb-1.17.0', 'https://xcb.freedesktop.org/dist/libxcb-1.17.0.tar.xz', '599ebf9996710fea71622e6e184f3a8ad5b43d0e5fa8c4e407123c88a59a6d55'), ('xcb-util-wm-0.4.2', 'https://xcb.freedesktop.org/dist/xcb-util-wm-0.4.2.tar.xz', '62c34e21d06264687faea7edbf63632c9f04d55e72114aa4a57bb95e4f888a0b'), ('xcb-util-errors-1.0.1', 'https://xcb.freedesktop.org/dist/xcb-util-errors-1.0.1.tar.xz', '5628c87b984259ad927bacd8a42958319c36bdf4b065887803c9d820fb80f357'), ('libX11-1.8.13', 'https://xorg.freedesktop.org/archive/individual/lib/libX11-1.8.13.tar.xz', '69606f485c2c07c14ef64f75b7bb326d48587af33795d9ab3e607c0b5f94f11c'), ('libxkbfile-1.2.0', 'https://xorg.freedesktop.org/archive/individual/lib/libxkbfile-1.2.0.tar.xz', '7f71884e5faf56fb0e823f3848599cf9b5a9afce51c90982baeb64f635233ebf'), ('libXfont2-2.0.9', 'https://xorg.freedesktop.org/archive/individual/lib/libXfont2-2.0.9.tar.xz', 'f042a370666815e7b941e9b7019024755bd1c6c2954afbfa515af378251799e2'), ('libfontenc-1.1.0', 'https://xorg.freedesktop.org/archive/individual/lib/libfontenc-1.1.0.tar.gz', '8597e761b7e3624fbf8de4b4f19b9927822f24eeaf15524dc5dbbaf6ae7d8dd1'), ('libxcvt-0.1.3', 'https://xorg.freedesktop.org/archive/individual/lib/libxcvt-0.1.3.tar.xz', 'a929998a8767de7dfa36d6da4751cdbeef34ed630714f2f4a767b351f2442e01'), ('libxshmfence-1.3.3', 'https://xorg.freedesktop.org/archive/individual/lib/libxshmfence-1.3.3.tar.xz', 'd4a4df096aba96fea02c029ee3a44e11a47eb7f7213c1a729be83e85ec3fde10'), ('freetype-2.14.1', 'https://download.savannah.gnu.org/releases/freetype/freetype-2.14.1.tar.xz', '32427e8c471ac095853212a37aef816c60b42052d4d9e48230bab3bdf2936ccc'), ('xkbcomp-1.5.0', 'https://xorg.freedesktop.org/archive/individual/app/xkbcomp-1.5.0.tar.xz', '2ac31f26600776db6d9cd79b3fcd272263faebac7eb85fb2f33c7141b8486060'), ('libepoxy-1.5.10', 'https://github.com/anholt/libepoxy/archive/refs/tags/1.5.10.tar.gz', 'a7ced37f4102b745ac86d6a70a9da399cc139ff168ba6b8002b4d8d43c900c15'), ('xkeyboard-config-2.48', 'https://xorg.freedesktop.org/archive/individual/data/xkeyboard-config/xkeyboard-config-2.48.tar.xz', 'b77041324f0109f77161ee43743fe04baa485866af8460d31e476ad3f7648fd5'), ('xwayland-24.1.13', 'https://www.x.org/releases/individual/xserver/xwayland-24.1.13.tar.xz', '173aea3d6f79609164c04528e1c8e4c9b60fcd59391c3c9dad4667297d727fb6')]:
    archive = sources / 'downloads' / (name + next(suffix for suffix in ('.tar.gz', '.tar.xz', '.tar.bz2') if url.endswith(suffix)))
    if not archive.exists(): urllib.request.urlretrieve(url, archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest: raise SystemExit('Checksum mismatch: ' + str(archive))
