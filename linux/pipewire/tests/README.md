# DSP mixer lifetime check

`mixer-lifetime.c` loads the pinned stock SPA mixer on the host. It mixes two
independent float streams, pauses the graph, detaches/unmaps the first stream,
and verifies that the other still produces the expected samples. It uses a
synchronous test loop and no sockets, Android, or audio hardware. This is a
buffer-ownership regression test, not a reproduction of the original tombstone.

With a host build of the pinned PipeWire source:

```sh
cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter \
  -I"$PW_SOURCE/spa/include" mixer-lifetime.c -ldl -o /tmp/mixer-lifetime
/tmp/mixer-lifetime "$PW_HOST_BUILD/spa/plugins/audiomixer/libspa-audiomixer.so"
```

Do not run the host test against an Android DSO. APK staging must include
`libspa-audiomixer.so`, and `maton.conf` must map `audio.mixer.*` to its flat
library name. `pipewire_android.c` also checks the DSP factory at startup so
missing or stale assets disable audio rather than selecting a non-mixing sink.
