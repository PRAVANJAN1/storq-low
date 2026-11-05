# Storq Core and Storage

Storq is a modular C++20 database reliability engineering foundation. The core
provides lifecycle management, configuration validation, structured logging,
typed errors and results, module registration, capabilities, version
information, common utilities, and a backend-neutral storage abstraction.
Database formats, storage engines, and fault injection are outside the current
modules.

## Build

Configure and build with CMake 3.20 or newer:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Tests and examples are enabled by default.

Run `build/storq_basic_runtime` and `build/storq_storage` on Linux, or
`build\Release\storq_basic_runtime.exe` and `build\Release\storq_storage.exe`
on Windows with a multi-configuration generator.

To select a shared library, configure with `-DSTORQ_BUILD_SHARED=ON`. Use
`-DSTORQ_BUILD_TESTS=OFF` or `-DSTORQ_BUILD_EXAMPLES=OFF` to disable those
targets.

## Core API

Include `<storq/storq.hpp>` and link against `Storq::core`. Runtime configuration
is validated before initialization. Modules register in insertion order, start
in that order, and stop in reverse order. A runtime instance is initialized at
most once; shutdown before initialization and repeated shutdown are safe no-ops.
Failed module initialization rolls back successfully started modules; a module
is responsible for cleaning up any partial work when its own initialization
fails. Logger methods return `Result<void>` so output failures are observable.

Only capabilities implemented by this core are advertised. Module-specific
capabilities can be registered through `Runtime::capabilities()`.

The install exports `Storq::core` for CMake consumers using
`find_package(Storq CONFIG REQUIRED)`.

## Storage API

The storage API is in `<storq/storage/storage.hpp>` and is also included by the
umbrella header. `StorageManager` creates native files, bounded or unbounded
in-memory files, and snapshots. A custom `IStorageBackend` can be supplied with
`create_storage`, leaving higher-level callers independent of backend type.
Register a `StorageModule` with a `Runtime` before runtime initialization to
advertise storage capabilities and close tracked resources on shutdown.

Native operations are serialized per backend, use `pread`/`pwrite` on POSIX and
explicit file positioning on Windows, and do not share an implicit OS file
offset across operations. Memory backend reads may run concurrently and memory
modifications are serialized; calls through `StorageFile` are serialized to
coordinate the sequential cursor and snapshots. Read operations return the
available byte count at EOF. Writes complete the requested span or report an
error. `sync()` uses `fsync` on POSIX and `FlushFileBuffers` on Windows;
read-only sync is a no-op. Success requests an OS-level flush but cannot promise
storage hardware or controller durability, and does not synchronize the parent
directory entry. Memory sync is a no-op because its contents are volatile.
POSIX `exclusive_access` uses an advisory file lock, while Windows opens the
handle without sharing. The permissions option applies to POSIX file creation;
Windows uses inherited filesystem ACLs.

Snapshot copying holds the source `StorageFile` operation lock and reports
consistency relative to operations through that handle. It is not atomic with
respect to other handles or processes modifying the same path, and does not
provide database transaction consistency. Native snapshot destinations use
`CreateNew` and are never overwritten. Observer lists are captured at operation
start; observers registered later do not receive that operation, while an
observer removed during it still receives its completion callback. Callbacks
run in registration order outside file and registry locks; observer exceptions
are isolated from storage operations.
