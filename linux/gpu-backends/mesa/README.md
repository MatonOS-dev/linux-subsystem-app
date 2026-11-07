# Mesa GPU backend (planned)

Linux apps run a stock glibc Mesa driver (RADV, radeonsi, Iris, ANV, ...) directly on
the GPU render node from inside the stub, the same device Android's own driver opens
in-process. Mesa still ships as our own GL Flatpak extension, built here; the
forwarder backend keeps its own extension in ../forwarder/gl-maton.

Presentation: the compositor imports the client's dma-buf with Android EGL
(EGL_EXT_image_dma_buf_import) and copies it into the window's AHardwareBuffer.

First spike: stock RADV `vulkaninfo` plus a clear-colour test in a stub on the
`-g native` VM, to check render node access and ioctls from untrusted_app.

The previous approach (Vulkan forwarder ICD into Android's driver) lives in
../forwarder and is kept, not built into anything new.
