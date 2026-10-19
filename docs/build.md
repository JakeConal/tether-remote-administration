# tether build notes

This project is built with CMake and vcpkg (manifest mode) on Windows and Linux.

## Build

```sh
# one-time on a fresh machine: bootstrap a vcpkg checkout of your own
#   git clone https://github.com/microsoft/vcpkg
#   <vcpkg>/bootstrap-vcpkg.(bat|sh)

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build
ctest --test-dir build --output-on-failure
```

GitHub Actions (`.github/workflows/ci.yml`, added in Phase 1 task 1.6) runs the
same sequence on both platforms using the runner-provided vcpkg.

## Test framework decision (Phase 1 task 1.4): doctest

**Chosen: doctest.** Rationale (one paragraph): The framework pick had to
resolve the design's open question (Catch2 v3 vs doctest) for a codebase whose
test suite is planned to grow rapidly through the P3 protocol boundary work.
doctest is a single-header library, so its integration cost is essentially
zero: no compiled framework library on the critical path, and compile times for
test translation units stay in the sub-second range, while Catch2 v3 requires
building and linking a compiled static library per binary - roughly an order
of magnitude slower per TU on a compiler-fresh CI runner. Both frameworks
satisfy the project's actual-testing requirement (protocol-boundary unit
tests wired into CTest), both are first-class vcpkg ports that resolve in
manifest mode, and both offer the same assertion/checking semantics we need
for the protocol work, so the differentiator is compile-time cost and
integration simplicity - both favoring doctest. We integrate it as a vcpkg
manifest dependency (`doctest`) consumed through `find_package(doctest
CONFIG REQUIRED)` and the `doctest::doctest` target, with the framework's
entry-point TU defining `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` (doctest's
single-header mode), and tests registered via plain `add_test()` calls so
CTest drives them identically on Windows and Linux.
