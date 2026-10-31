# SIGSYS trap cost investigation

The table uses the default unmarked temporary-directory fixture in
`tests/trap-cost.sh` (10,000 iterations for cwd/close,
5,000 for lookups/open/readlink, 1,000 for unlink/mkdir, and 10 for link-store
linking). The i386 payload is linked to host glibc using the same freestanding
entry pattern as the existing i386 tests. Proxied means `linux-layer-v3` or
`linux-layer-v3-i386` with a `/bench` map. Values are microseconds per call;
the before/after pair compares `9648bb1` with the counter bookkeeping guard.

| Operation | x86_64 native | x86_64 proxied | i386 native | i386 proxied |
|---|---:|---:|---:|---:|
| getcwd | 0.790 → 0.618 | 3.588 → 3.225 | 0.755 → 0.734 | 4.324 → 4.177 |
| close(-1) | 0.350 → 0.327 | 0.408 → 0.414 | 0.404 → 0.375 | 0.649 → 0.591 |
| access | 1.446 → 1.559 | 9.268 → 8.942 | 1.329 → 1.322 | 7.681 → 7.453 |
| stat | 1.715 → 1.712 | 9.671 → 9.397 | 1.973 → 1.617 | 8.704 → 8.449 |
| openat + close | 2.517 → 2.617 | 13.220 → 13.554 | 2.719 → 2.842 | 13.030 → 12.660 |
| readlink | 1.614 → 1.461 | 9.070 → 8.340 | 1.868 → 1.592 | 9.021 → 8.674 |
| unlinkat + recreate | 15.727 → 16.104 | 53.341 → 47.827 | 15.975 → 16.338 | 46.010 → 46.958 |
| linkat + unlink | 10.886 → 11.066 | 37.564 → 36.534 | 10.757 → 11.186 | 33.182 → 34.756 |
| mkdirat + rmdir | 29.216 → 27.427 | 92.528 → 88.760 | 27.538 → 35.610 | 92.666 → 91.592 |

The change guards per-trap raw-syscall diagnostics that are only meaningful in
`PROXY_RAW_COUNTER` builds. Ordinary builds were clearing and scanning a
`PROXY_SC_count` array on trap entry/exit. This removes that fixed work. The
i386 close delta is small and within host-run noise; it does not explain the
approximately 49 µs observed inside the VM. The host proxy adds about 0.2 µs
for i386 `close(-1)`; signal delivery/return and VM exits remain the likely
dominant fixed cost in that measurement. A direct host microbenchmark cannot
separate QEMU/KVM exits from the proxy's signal path.

The generic mapped-directory benchmark does not reproduce Steam's 150 µs to
1 ms mutation costs. Run `MATON_TRAP_COST_STORE=1 bash
tests/trap-cost.sh` to use a marked `.v3-store-root` fixture and exercise the
indexed hardlink implementation: link publication converts the source to a
tracked object and updates placeholder, count, and journal metadata. In that
mode, the first proxied link operation measured 72–87 ms on this host; this is
first-time conversion and is not comparable to the repeated plain-directory
link row above. The journal writer writes and renames metadata but does not
call `fsync`; the large cost is therefore not explained by an explicit proxy
fsync. More profiling on the actual VM/Steam tree is needed to apportion
component walks, store locking/recovery, and VM exits. No
correctness-sensitive store caching or bypass was added.

## Verification

- `bash tests/trap-cost.sh`: completed on x86_64 and i386, native and proxied.
- `MATON_CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/r17-host-chrome MATON_APPLY_EXTRA_TEST_OUT=/mnt/data/aosp/out/pc-logs/r17-host-extra bash linux/compat-proxy/tests/run.sh`: exit 0; log at `/mnt/data/aosp/out/pc-logs/trap-cost-suite.log`.
- Raw-counter build and trace confirmed the path handler makes multiple raw
  calls. The startup-inclusive count is not sufficient to assign the VM's
  per-operation latency to a specific syscall.

## Follow-up ideas, ranked

1. Capture per-operation raw syscall counts and handler sections in the Steam
   VM with the same image and filesystem state; the host numbers do not explain
   the reported VM costs.
2. Profile a warm indexed-store hardlink separately from first-time source
   conversion, including lock/recovery and component-walk syscall counts.
3. If those profiles identify repeated metadata probes, reduce only probes
   proven redundant within one path walk; retain cross-process crash recovery.
