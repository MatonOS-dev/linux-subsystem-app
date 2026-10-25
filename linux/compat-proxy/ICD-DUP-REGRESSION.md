# ICD duplicate host regression — fd metadata cleanup

The fd-cleanup merge exposed a descriptor-reuse bug in the non-EROFS VFS
pathname metadata. The proxy tracks names by fd so `/proc/self/fd/N` can be
translated back to the virtual path. Since ordinary closes now bypass proxy
dispatch, a stale name can survive after the kernel closes and reuses that fd.

The GL ICD probe opens each of three aliases with `O_PATH`, resolves the proc
fd path, and closes it. Later, `opendir("/usr/share/vulkan/icd.d")` follows a
runtime symlink to a generated GL merge directory. If its native directory
open reuses one of the prior fd numbers, `vfs_fd_name` can return the former
ICD manifest path for that directory descriptor. The libc `opendir` flow then
observes inconsistent pathname metadata and can fail with `ENOTDIR`.

This repository's EROFS single-image design removed indexed EROFS metadata and
tracked EROFS descriptors. The fix therefore retires the generic fd-name
association: after a successful native path open, `path_call()` calls
`vfs_fd_close(fd)` before recording the new pathname. This clears any stale
association even though the ordinary close ran natively. Duplication continues
to clear the target association before copying a source name.

`tests/icd-dup-host.py` covers the regression, including the real bwrap pivot
when available. It checks realpath/proc-fd aliases, directory opens, stat and
link metadata, and loader duplicate discovery behavior where Vulkan tools are
installed.
