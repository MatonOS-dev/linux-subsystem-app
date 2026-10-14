#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ${COMPAT_GPU_SKIP_BUILD:-0} != 1 ]]; then ./build.sh; fi
cc -no-pie -O2 -Wall -Wextra -Werror tests/gpu-sysfs.c build/liblinux-layer-v3.a -o build/gpu-sysfs
sed 's/\<proxy_raw\>/gpu_test_raw/g' arch/x86_64/raw.S >build/gpu-raw.S
cc -nostdlib -static -no-pie -mcmodel=large -Wl,-Ttext-segment=0x600000000000 -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror loader.c proxy.c vfs.c links.c net.c sysvipc.c tests/gpu-raw.c build/gpu-raw.S entry.S -o build/gpu-loader
fixture=$(mktemp -d /mnt/data/aosp/out/gpusys-test.XXXXXX)
trap 'rm -rf "$fixture"' EXIT
mkdir "$fixture/dri" "$fixture/tmp" "$fixture/props"
touch "$fixture/dri/renderD129" "$fixture/dri/card2"
python3 - "$fixture" <<'PY'
import struct,sys,pathlib
root=pathlib.Path(sys.argv[1])
def area(mode):
    data=bytearray(20)
    nodes={():0}
    def alloc(value):
        offset=len(data);data.extend(value);data.extend(b'\0'*(-len(data)%4));return offset
    props={'vendor':'0x8086','device':'0x5912','driver':'i915','render_node':'/dev/dri/renderD129','mode':mode}
    for suffix,value in props.items():
        name='vendor.maton.graphics.'+suffix
        parent=()
        for token in name.split('.'):
            child=parent+(token,)
            if child not in nodes:
                offset=alloc(struct.pack('<5I',len(token),0,0,0,0)+token.encode()+b'\0')
                nodes[child]=offset
                link=nodes[parent]+16
                sibling=struct.unpack_from('<I',data,link)[0]
                while sibling:
                    n=struct.unpack_from('<I',data,sibling)[0];other=data[sibling+20:sibling+20+n].decode()
                    link=sibling+(8 if (len(token),token)<(len(other),other) else 12)
                    sibling=struct.unpack_from('<I',data,link)[0]
                struct.pack_into('<I',data,link,offset)
            parent=child
        offset=alloc(struct.pack('<I',len(value)<<24)+value.encode().ljust(92,b'\0')+name.encode()+b'\0')
        struct.pack_into('<I',data,nodes[parent]+4,offset)
    return struct.pack('<4I',len(data),0,0x504f5250,0xfc6ed0ab)+b'\0'*112+data
(root/'hardware').write_bytes(area('hardware'))
(root/'software').write_bytes(area('software'))
(root/'malformed').write_bytes(b'bad')
corrupt=bytearray(area('hardware'));struct.pack_into('<I',corrupt,144,0xfffffff0)
(root/'corrupt').write_bytes(corrupt)
dirty=bytearray(area('hardware'));name=dirty.index(b'vendor.maton.graphics.vendor\0');serial=name-96
struct.pack_into('<I',dirty,serial,struct.unpack_from('<I',dirty,serial)[0]|1)
(root/'dirty').write_bytes(dirty)
(root/'missing-device').write_bytes(area('hardware').replace(b'vendor.maton.graphics.device',b'vendor.maton.graphics.xxxxxx'))
PY
prop=u:object_r:vendor_maton_graphics_prop:s0
cp "$fixture/hardware" "$fixture/props/$prop"
TMPDIR="$fixture/tmp" timeout 30 build/gpu-loader --android-filter --map "$fixture/props" /dev/__properties__ --map "$fixture/dri" /dev/dri "$PWD/build/gpu-sysfs" >build/gpu-sysfs.out 2>build/gpu-sysfs.log
cat build/gpu-sysfs.out
if command -v bwrap >/dev/null; then
    TMPDIR="$fixture/tmp" timeout 30 build/gpu-loader --android-filter --map "$fixture/props" /dev/__properties__ --map "$fixture/dri" /dev/dri "$(command -v bwrap)" --ro-bind / / --proc /proc --dev /dev "$PWD/build/gpu-sysfs" >build/gpu-bwrap.out 2>build/gpu-bwrap.log
    cat build/gpu-bwrap.out
    printf 'PASS generated GPU tree survives bwrap recursive root bind/pivot\n'
fi
for mode in software malformed corrupt dirty missing-device missing; do
    args=()
    if [[ $mode != missing ]]; then cp "$fixture/$mode" "$fixture/props/$prop"; args+=(--map "$fixture/props" /dev/__properties__); fi
    TMPDIR="$fixture/tmp" timeout 15 build/gpu-loader --android-filter "${args[@]}" --map "$fixture/dri" /dev/dri /bin/sh -c 'test ! -e /sys/dev/char/226:129/device/vendor' >"build/gpu-$mode.out" 2>"build/gpu-$mode.log"
    printf 'PASS %s property fallback\n' "$mode"
done

cp "$fixture/hardware" "$fixture/props/$prop"
touch "$fixture/dri/card3"
TMPDIR="$fixture/tmp" timeout 15 build/gpu-loader --android-filter --map "$fixture/props" /dev/__properties__ --map "$fixture/dri" /dev/dri /bin/sh -c 'test -e /sys/dev/char/226:129/device/vendor && test ! -e /sys/dev/char/226:2' >build/gpu-multi.out 2>build/gpu-multi.log
printf 'PASS ambiguous primary card omitted, selected render retained\n'
rm "$fixture/dri/card3" "$fixture/dri/renderD129"
TMPDIR="$fixture/tmp" timeout 15 build/gpu-loader --android-filter --map "$fixture/props" /dev/__properties__ --map "$fixture/dri" /dev/dri /bin/sh -c 'test ! -e /sys/dev/char/226:129/device/vendor' >build/gpu-no-node.out 2>build/gpu-no-node.log
printf 'PASS absent render node fallback\n'
