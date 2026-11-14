# Hosted verification

The source is in the private
[happenings-dk/connect-cc](https://github.com/happenings-dk/connect-cc) repository,
with GitHub-hosted Linux, macOS and Windows runners. This report distinguishes
executed results from pending checks and records the source used for each.

## Current runtime verification

The corrected runtime is `72d27398a19fb7975a7624340f6787b3ec4a374b`.
Windows MSVC passes at that revision. The refreshed non-Windows checks use
`b9d8cbf68c57c8cc47613024466ef553c036af14`, with identical runtime and test
sources; its only additional change is benchmark dependency caching/build time.

Each compiler lane builds the project, runs all **222 CTest entries**, runs Go
checks, and compiles/runs independent installed consumers and all **21 public
headers**. The ten official profiles contain **8,698 client/server cases**,
nested within the CTest entries. [CONFORMANCE.md](CONFORMANCE.md) records the
suite revision and profiles.

| Lane | Hosted result | Evidence |
| --- | --- | --- |
| Windows MSVC 19.51 | **Pass, 222/222**, consumers and headers | [Run](https://github.com/happenings-dk/connect-cc/actions/runs/43532212334) |
| macOS Apple Clang | **Pass, 222/222**, consumers and headers | [Run](https://github.com/happenings-dk/connect-cc/actions/runs/43533030051) |
| Linux GCC x86-64 | **Pass, 222/222**, consumers and headers | [Run](https://github.com/happenings-dk/connect-cc/actions/runs/43533033665) |
| Linux Clang x86-64 | **Pass, 222/222**, consumers and headers | [Run](https://github.com/happenings-dk/connect-cc/actions/runs/43533038312) |
| Linux GCC arm64 | **Pass, 222/222**, consumers and headers | [Run](https://github.com/happenings-dk/connect-cc/actions/runs/43533042437) |
| Linux ASan/UBSan | Refreshed run in progress | [Run](https://github.com/happenings-dk/connect-cc/actions/runs/43533046764) |
| Linux TSan | **Pass, 222/222**, consumers and headers | [Run](https://github.com/happenings-dk/connect-cc/actions/runs/43533051124) |

Independent Linux GCC/Clang, ASan/UBSan and TSan runs of the corrected source
already pass 222/222 and installed consumers/headers. macOS normal and ASan/UBSan
also pass 222/222. Their source manifest matches all 132 runtime/test/package
files against the verification workspace; logs are retained under
`build/verification/tcp-shutdown/` and `build/verification/tcp-shutdown-*.log`.

All six non-Windows hosted lanes previously passed at `c4bf581`, before the
shutdown correction: [GCC](https://github.com/happenings-dk/connect-cc/actions/runs/43529834443),
[Clang](https://github.com/happenings-dk/connect-cc/actions/runs/43529837747),
[arm64](https://github.com/happenings-dk/connect-cc/actions/runs/43529841530),
[macOS](https://github.com/happenings-dk/connect-cc/actions/runs/43529830282),
[ASan/UBSan](https://github.com/happenings-dk/connect-cc/actions/runs/43529845399),
and [TSan](https://github.com/happenings-dk/connect-cc/actions/runs/43529848684).
That historical baseline is distinct from the refreshed runs above.

These manual runs select individual compiler lanes without reducing their test
coverage. Push/PR runs retain the complete matrix. Parser fuzzing is a separate
job; its evidence is below. Cached dependencies are ABI-checked by vcpkg;
project compilation, tests and installed consumers always run.

## Additional evidence

- [Hosted vcpkg and Conan jobs](https://github.com/happenings-dk/connect-cc/actions/runs/43524141109)
  passed at `a705cb6`, including all 222 Conan tests and independent consumers.
  Both packages were rebuilt locally on Linux against `72d2739`: Conan passes
  222/222 and both packages pass installed examples and all 21 public headers.
- [Hosted parser fuzzing](https://github.com/happenings-dk/connect-cc/actions/runs/43524141112/job/132225569915)
  completed 2,903,286 inputs in 121 seconds without a finding at `a705cb6`.
  A fresh Linux ASan/UBSan campaign on `72d2739` completed 7,099,792 inputs
  in 121 seconds without a finding. See [FUZZING.md](FUZZING.md).
- The [hosted benchmark run](https://github.com/happenings-dk/connect-cc/actions/runs/43524141262)
  at `a705cb6` exhausted 60 minutes compiling native gRPC dependencies, before
  RPC measurement. `b9d8cbf` adds dependency caching and a 90-minute job budget;
  the [replacement run](https://github.com/happenings-dk/connect-cc/actions/runs/43532346830)
  is in progress. The measured Linux run at `72d2739` passes all 90
  scenarios and 449,640 RPCs with zero errors. Raw results, source fingerprints,
  environment and measurement limits are in [BENCHMARKS.md](BENCHMARKS.md).
- Independent Linux GCC 14, Clang 18, ASan/UBSan and instrumented TSan runs at
  `c4bf581` pass all 222 tests and installed consumers/headers. macOS normal
  tests pass 222/222. The macOS sanitizer run passed all C++/protocol tests;
  its Go-generator check failed on missing files in the shared Go cache, then
  passed unchanged with the isolated `GOCACHE=$PWD/build/go-cache`.
  Logs and exact source fingerprints are retained under
  `build/verification/msvc-coroutine/` and `build/verification/msvc-coroutine*.log`.

## Failures found and corrected

The initial import was `fd5f5d7`. Its full hosted run is retained as historical
failure evidence, not represented as a successful run.

1. AppleClang 17/libc++ did not provide `std::jthread`. A callback test now uses
   `std::thread`; both threads were already explicitly joined. Its concurrency
   and cancellation assertions are unchanged. The corrected macOS job passes.
2. The original TSan job passed every official profile and reported no race,
   but `Interop.Go` exceeded its outer 90-second limit during a cold Go race
   compilation. CMake now compiles that helper during the build, then CTest
   executes it directly. Assertions and the runtime limit are unchanged.
   The [corrected job](https://github.com/happenings-dk/connect-cc/actions/runs/43521855870/job/132217907387)
   passes all 222 tests; interoperability takes 1.42 seconds.
3. MSVC rejected nghttp2's legacy POSIX `ssize_t` declarations. Both HTTP/2
   engines now use `nghttp2_ssize`, `nghttp2_data_provider2` and the matching
   request/response/memory I/O APIs. Private `NGHTTP2_NO_SSIZE_T` prevents old
   declarations on every platform. Build and installed package configurations
   require nghttp2 1.61 or newer.
4. After that fix compiled, MSVC 19.51 crashed internally with C1001 in its
   optimizer at the nested coroutine lambda in `Server::HandleHttpStream`.
   The [first occurrence](https://github.com/happenings-dk/connect-cc/actions/runs/43522666372/job/132220632959)
   and [independent reproduction](https://github.com/happenings-dk/connect-cc/actions/runs/43524141112/job/132225623503)
   retain the compiler diagnostics. `c4bf581` moves the unary-body coroutine
   into a named internal helper and uses a non-coroutine capture adapter.
   Optimization remains enabled; no protocol behavior or assertion was removed.

5. The first executable MSVC run passed 216/222 entries. Failures in unary
   loopback, generated bindings, interceptor integration and the HTTP/1.1 server
   profile exposed peer resets at socket close. The verified correction calls
   TCP `shutdown_send` before close, without masking errors or changing status
   assertions. Its first regression setup incorrectly injected reserved
   `connection` metadata; the corrected regression invokes the public
   `Shutdown` operation after a large complete response. That invalid-fixture
   hosted run was canceled and replaced by the exact corrected revision.
   The corrected MSVC Release run passes 222/222 with zero failures/skips,
   including every official profile, plus installed consumers and headers.

One early Windows dispatch picked up the preceding revision while an SSH upload
was delayed. It was canceled as stale; the current run's SHA was verified before
using its result. A vcpkg local rerun also refused to overwrite an existing
output overlay; a fresh output directory completed successfully without a source
change. These setup failures are separate from protocol test results.

Artifacts retain JUnit results, CTest logs and CMake diagnostics for 14 days.
Downloaded copies and complete job logs are preserved in the development
workspace under `build/verification/github/` and `build/verification/`.
These finite tests are compatibility evidence, not upstream endorsement or
proof that every possible input has been tested. Pending jobs do not count as
passes. The evidence above applies to the stated source and platform matrix.
