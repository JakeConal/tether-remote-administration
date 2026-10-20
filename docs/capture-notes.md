# Capture family notes: own-lib X11 vs shelling to screenshot utilities

Task 6.5 required benchmarking the own X11 implementation against shelling to
existing utilities and recording the decision plus the measured numbers here.

## Decision

**The own-library X11 implementation is the production path.** It lives in
`lib/syscalls/capture.cpp`: `XOpenDisplay` -> `XCopyArea` of the root window
onto a temporary pixmap -> `XGetImage` of that pixmap -> BGR normalization ->
PNG encode with opencv imgcodecs (`image_png.hpp`). Reasons, in order:

1. **Correctness on composited roots.** A direct root-window `XGetImage`
   fails with `BadMatch` on XWayland/WSLg (composited 32-bit root). The
   exact failure was observed twice: once by the first prototype of this
   implementation, and independently by `xwd -root`, which died with
   `BadMatch (invalid parameter attributes) ... Major opcode 73 (X_GetImage)`
   in the benchmark run below. The temporary-pixmap copy (`XCopyArea` then a
   plain same-depth `XGetImage`) sidesteps it and is what shipped.
   `swallow_x_errors` additionally guards the grab so a bad grab becomes a
   structured failure instead of Xlib's process termination.
2. **No external-tool dependency on the agent host.** Shelling needs
   `scrot` (imlib2) or `xwd` (+ netpbm for a PNG) present and in PATH, and
   would re-splice parameters into command text - exactly the pattern the
   remote-commands spec forbids. The own path needs only libX11.
3. **In-process deliverable.** The own path hands a byte-complete PNG back
   inside the process (no temp files, no cleanup); the shell candidates
   write a file to /tmp as a side effect.
4. **Measured performance.** Roughly 2.3x faster than `scrot` end to end on
   the reference machine (median 34 ms vs 79 ms; the shell numbers include
   their disk write, the own number is the full grab+encode).

`screenshot` on Windows remains the GDI `BitBlt`/`GetDIBits` path,
PNG-encoded through the same `image_png` helper. Both paths are exercised by
`capture-family-test` on their CI leg (Windows desktop session / Linux
under `xvfb`).

## Benchmark note (measured numbers)

Environment: WSL2 + WSLg (XWayland), Ubuntu 24.04-base guest, DISPLAY :0,
1920x1080 root, 15 iterations per candidate, tool
`tests/capture_bench.cpp` (built as `tether-capture-bench`; NOT a ctest -
it is run manually on a desktop session), 2026-10-20.

| Candidate                                | min    | median | max    |
|------------------------------------------|--------|--------|--------|
| own-lib (XCopyArea + XGetImage + opencv PNG, in-process) | 29 ms | 34 ms | 42 ms |
| `scrot -o /tmp/...png` (imlib2 -> PNG on disk)          | 76 ms | 79 ms | 96 ms |
| `xwd -root -silent -out`                                 | unavailable (exits 1) |  |  |

The `xwd` candidate cannot serve as a screenshot path on composited roots:
its raw root `XGetImage` fails with

```
X Error of failed request:  BadMatch (invalid parameter attributes)
  Major opcode of failed request:  73 (X_GetImage)
```

which matches the first prototype's failure and motivates the
temporary-pixmap design above. The own-lib run produced a decodable PNG
(`file` on a scrot sample of the same screen: `PNG image data, 1920 x 1080,
8-bit/color RGB`); the capture-suite test decodes the produced bytes with
`cv::imdecode` and asserts real sizes/pixels rather than just non-emptiness.
