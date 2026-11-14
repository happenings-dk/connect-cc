# Hosted verification

The source is in the private
[happenings-dk/connect-cc](https://github.com/happenings-dk/connect-cc) repository,
with GitHub-hosted Linux, macOS and Windows runners. This report distinguishes
executed results from pending checks and records the source used for each.

## Support expansion verification

The final runtime revision is `d7d93b4`. It adds the deadline saturation and
post-DNS cancellation guard described below. Local full, fatal-UBSan and Lite
checks pass; its final hosted matrix is pending. The baseline evidence below
must not be read as a hosted pass for those last runtime changes.

The expansion baseline is `8a6078a9cbca881bd9d81d1066453e12527f54c7`.
It adds Editions 2023–2024, Protobuf Lite, managed connections, native trust,
and bounded TLS close-notify. Revision `3d6a6bc` changes the managed-transport
regression fixture and documentation; its library sources are identical.

Local macOS normal and ASan/UBSan runs pass **236/236 CTest entries**, including
all ten official profiles and their **8,698 client/server cases**. The separate
Lite-only suite passes **13/13**. The installed README example returns
`Hello, World`; the native-trust HTTPS probe passes. Link inspection confirms
that the Lite consumer uses `libprotobuf-lite` without the full runtime.

| Hosted lane | Source | Result | Evidence |
| --- | --- | --- | --- |
| Linux GCC x86-64 | `8a6078a` | Pass: 236 full, 13 Lite, installed consumers, native trust | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43578682133/job/132398536101) |
| Linux Clang x86-64 | `8a6078a` | Pass: 236 full, 13 Lite, installed consumers, native trust | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43578682133/job/132398536096) |
| Linux GCC ARM64 | `8a6078a` | Pass: 236 full, 13 Lite, installed consumers, native trust | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43578682133/job/132398536122) |
| macOS Apple Clang | `8a6078a` | Pass: 236 full, 13 Lite, installed consumers, native trust | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43578682133/job/132398536113) |
| Linux TSan | `8a6078a` | Pass: 236 full, 13 Lite, installed consumers | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43578682133/job/132398536118) |
| Linux ASan/UBSan | `8a6078a` | 236 full and 13 Lite test exits pass, but retained OpenSSL UBSan diagnostics invalidate a clean result; strict rerun required | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43578682133/job/132398536083) |
| Windows MSVC | `3d6a6bc` | Pass: 236 full, 13 Lite, installed consumers, native trust | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43580628606/job/132404425262) |

Installed full-runtime checks compile all **22 public headers** independently.
Windows includes its platform headers first to exercise macro compatibility.
Lite checks include an independently installed consumer and symbol/link evidence.
The latest PR state is available on [PR #1](https://github.com/happenings-dk/connect-cc/pull/1/checks).

- [Full and Lite vcpkg/Conan packages](https://github.com/happenings-dk/connect-cc/actions/runs/43576664606)
  pass at `b55d2d9`, including the full 234-entry Conan protocol suite and
  independent consumers. The [refresh at `8a6078a`](https://github.com/happenings-dk/connect-cc/actions/runs/43578682163)
  passes both managers, including the full 236-entry Conan suite and the
  independent Lite consumer.
- [Parser fuzzing at `8a6078a`](https://github.com/happenings-dk/connect-cc/actions/runs/43578682133/job/132398499957)
  completes **4,872,082 inputs in 121 seconds** without a finding, with
  ASan/UBSan-instrumented target dependencies. An earlier expansion campaign
  at `b55d2d9` completed 3,636,382 inputs in 121 seconds without a finding.
  Both retained fuzz logs were checked for sanitizer diagnostics; the TLS test
  diagnostics above do not appear in the parser-only campaign. The final rerun
  additionally makes any UBSan diagnostic fatal.
- The [benchmark correctness smoke run at `8a6078a`](https://github.com/happenings-dk/connect-cc/actions/runs/43578682139)
  is running. The measured performance dataset below remains explicitly tied
  to its earlier runtime revision.

The following sections preserve the original acceptance evidence. Their
222-entry counts do not describe the expanded suite above.

## Initial-scope verification (historical)

The corrected runtime is `72d27398a19fb7975a7624340f6787b3ec4a374b`.
Windows MSVC passes at that revision. The refreshed non-Windows checks use
`b9d8cbf68c57c8cc47613024466ef553c036af14`, with identical runtime and test
sources; its only additional change is benchmark dependency caching/build time.

In the historical baseline below, each compiler lane built the project and ran **222 CTest entries**, runs Go
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
| Linux ASan/UBSan | **Pass, 222/222**, consumers and headers | [Run](https://github.com/happenings-dk/connect-cc/actions/runs/43533046764) |
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
  passed. The measured Linux run at `72d2739` passes all 90
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

## Expansion regressions and corrections

1. The first expanded Windows build exposed `max` macro expansion in public
   deadline defaults. Parenthesized calls fix the headers; installed-header
   checks now include Windows headers first.
2. The first Linux ASan run found a repeated-field container overflow in the
   Editions round trip with an uninstrumented Protobuf parser. The dedicated
   ASan/UBSan dependency triplet keeps container annotations and message globals
   consistent. The [corrected run at `22fcb33`](https://github.com/happenings-dk/connect-cc/actions/runs/43577802196)
   passes 235 full test exits, 13 Lite test exits and installed consumers without
   disabling container checks. Retained logs subsequently exposed OpenSSL's
   generic-function UBSan diagnostics; this is not clean sanitizer evidence.
   [SANITIZERS.md](SANITIZERS.md) documents the dependency-only configuration
   correction and the final fatal-UBSan policy.
3. Windows at `8a6078a` passed all ten official profiles but timed out in the
   managed-dial fixture. Its 40 ms deadline could expire before TCP connected,
   leaving the fixture's listener waiting. The correction joins the listener
   on every path, explicitly tests pre-connect expiration, and observes a TLS
   ClientHello before checking stalled-handshake cancellation. It passes 20
   local repetitions and 10 under ASan/UBSan; the Windows rerun is listed above.
4. Reconfiguring an SSL context could retain previously loaded CA roots. A
   regression demonstrated a successful handshake after the CA should have
   been removed. Replacing the trust store now rejects that peer; normal and
   sanitizer suites include the regression.
5. Completed gRPC error trailers are preserved if the server subsequently
   resets the unfinished upload with CANCEL. A deterministic regression covers
   the ordering; Windows `8a6078a` passes the previously failing gRPC timeout
   profile, although its unrelated managed-fixture timeout prevented an
   overall pass for that job.
6. An extreme managed connect timeout overflowed during conversion from
   milliseconds to the steady clock's finer duration. The regression reproduced
   signed overflow under fatal UBSan. Connect and idle deadlines now saturate;
   the four TCP/TLS reuse tests exercise the maximum millisecond duration.
   Cancellation is also checked after DNS completion before TCP can reopen a
   canceled socket. Normal and fatal-UBSan local suites pass 236/236; Lite
   passes 13/13. Final hosted verification includes these changes.
