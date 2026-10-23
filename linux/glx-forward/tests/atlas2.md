PaperCuts atlas regression
=========================

The host ETR 0.8.4 menu trace (2026-10-23, software Mesa) grows its RGBA
font page from 128x128 to 256x256. Calls 2109020–2109059:

1. Gen/bind new texture; TexImage2D RGBA/UNSIGNED_BYTE with NULL pixels.
2. Clamp-to-edge, nearest filters, then switch both filters to linear.
3. Save READ/DRAW_FRAMEBUFFER_BINDING; GenFramebuffersEXT twice.
4. BindFramebufferEXT READ and DRAW; FramebufferTexture2DEXT attaches the
   old/new pages; CheckFramebufferStatusEXT checks both.
5. BlitFramebufferEXT copies 128x128 with COLOR_BUFFER_BIT/NEAREST.
6. Restore READ/DRAW binds, delete FBOs, flush, delete the old texture.
7. TexSubImage2D uploads a new 24x33 glyph at (0,92), then further glyphs.

No repeated 512/1024 font growth or PixelStorei/mipmap call appeared in
this menu transition. Other image textures have larger/non-square sizes.

atlas2.c exercises the decisive EXT-only FBO sequence with deterministic
colored glyphs and pixel checks, including a check that the EXT binds
actually changed both framebuffer bindings. atlas.c used core binds and
therefore did not cover this failure. The additional padded-row, alpha,
and copy fallback checks are retained from atlas.c.

The registry does not mark BindFramebufferEXT or BindRenderbufferEXT as
aliases of core binds: EXT supports implicit object creation. Generate
real desktop thunks for the EXT framebuffer-object extension rather than
inventing an alias. Previously glBindFramebufferEXT was absent from the
forwarder's proc table although all neighboring calls resolved.

Host: cc -O2 atlas2.c -lGL -lX11 -o /absolute/out/atlas2
       DISPLAY=:99 /absolute/out/atlas2
VM: use the same GLX/stub launch procedure as atlas.c/run129.py.

Host llvmpipe passes all pixel checks. VM failure-before/pass-after and
PaperCuts screenshot still require an available VM; see the agent report.
