# Storq Core

Storq Core is the C++20 runtime foundation for Storq. It provides lifecycle
management, configuration validation, structured logging, typed errors and
results, module registration, capabilities, version information, and common
utilities. Storage operations and database-specific functionality are outside
the scope of this module.

## Build

Configure and build with CMake 3.20 or newer:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Tests and the example are enabled by default. To run tests:

```sh
ctest --test-dir build --output-on-failure
```

Run the example with `build/storq_basic_runtime` on Linux, or
`build\Release\storq_basic_runtime.exe` on Windows with a multi-configuration
generator.

To select a shared library, configure with `-DSTORQ_BUILD_SHARED=ON`. Use
`-DSTORQ_BUILD_TESTS=OFF` or `-DSTORQ_BUILD_EXAMPLES=OFF` to disable those
targets.

## Public API

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
