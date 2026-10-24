# Linux app install concurrency

## Pipeline

`LinuxAppStoreService` authenticates and queues each store operation. An
install/update/repair creates its own staging ID and invokes
`InstallTransaction`. That transaction asks the Flatpak plan helper for the
root ref, runtimes, and related refs, then resolves origins and installed
reuse before it pulls content. Each non-reused ref has its own Flatpak
installation, home, temp, and run directories beneath that operation's
staging tree. After pull and OSTree verification, `StagedPacks` creates pack
artifacts; the service then commits package sessions in plan order so a
runtime package is installed before packages that declare it as a dependency.

Previously the service had one worker and rejected a second active install;
the transaction also pulled refs in a serial loop and packed dependencies as
part of their serial package-install loop. The service worker and per-plan
pull/pack phases now use pools bounded by `Runtime.availableProcessors()`.
PackageInstaller commits stay sequential within a plan because dependency
packages must be present before their dependants. Different operations can
commit concurrently, with a lock per package name to prevent same-package
sessions racing.

## Concurrency and locks

- An operation owns its cancellation flag, child-process set, work directory,
  callback/progress counters, and final result. Cancelling it destroys only
  its Flatpak children and does not cancel another operation's executor.
- A plan node waits for its declared dependency futures. A failed node causes
  its dependants to be skipped; independent branches still finish. Failure of
  the requested ref reports that operation as failed.
- Refs in one operation use separate Flatpak installations, so OSTree repo
  writes and deployments do not contend. All installations share the Flatpak
  system download cache under the host app's private work root. A process-wide
  monitor keyed by remote, ref, and commit serializes identical snapshot pulls;
  a later pull for that snapshot can reuse content-addressed objects from the
  cache while deploying into its own staging tree. The active transaction
  holds a `WorkDirs` lease on the cache so startup stale-work sweeping cannot
  remove it during a pull.
- PackageInstaller commits lock only the target package name. The operation
  worker lock is never held while waiting for a package-name lock. The
  foreground notification and wake lock are reference-counted across active
  transactions and removed after the last transaction completes.
- Progress byte totals and verified payload counts are atomic per operation;
  callback reporting is serialized and percent values stay monotonic.

The lock order is deliberately shallow: the shared pull-lock registry is held
only to retain/release a per-snapshot monitor; no filesystem or Flatpak work
runs under the registry monitor. The package lock is held only for that
package's install/session work. No operation lock is acquired while holding
either shared registry lock.

## Host checks and VM follow-up

`linux/stubgen/tests/PlanExecutorTest.java` covers dependency-before-dependent
execution, shared in-flight snapshot work, cancellation isolation, and a
failed ref skipping only dependants. The repository test entry point is
`tests/run.sh` with `MATON_AOSP_ROOT` set to the checkout root.

On the next fresh-image VM run, check that two simultaneous installs report
independent IDs and progress, cancelling one leaves the other running, a
shared runtime is fetched once (Flatpak/OSTree logs and cache activity),
dependency package commits precede the app commit, and a failed unrelated
related-ref does not stop an independent branch. Also check two operations
targeting the same package fail/retry cleanly instead of installing competing
sessions. No VM was used during implementation.
