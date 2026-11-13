# Hosted verification

The source is in the private
[happenings-dk/connect-cc](https://github.com/happenings-dk/connect-cc) repository,
created with Rasmus's authorization. Windows runs on GitHub's hosted MSVC
runner; access to the homelab Windows machine is no longer required.

## Recorded jobs

The initial source revision is `fd5f5d7073024f110b04d98475c0750ced9f4760`.
Each compiler lane runs the complete CTest suite, including the ten official
conformance profiles, Go checks and independent installed consumers/headers.
The 8,698 official cases are included within the 222 CTest entries, not added
to that entry count. Test profiles and the pinned suite revision remain in
[CONFORMANCE.md](CONFORMANCE.md).

| Lane | Result | Evidence |
| --- | --- | --- |
| Linux GCC x86-64 | Pass: 222 tests and installed consumers | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43515947046/job/132197811988) |
| Linux Clang x86-64 | Pass: 222 tests and installed consumers | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43515947046/job/132197812119) |
| Linux GCC arm64 | Pass: 222 tests and installed consumers | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43515947046/job/132197812170) |
| Linux ASan/UBSan | Pass: 222 tests and installed consumers | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43515947046/job/132197811990) |
| macOS Apple Clang | Pass after test portability fix: 222 tests and installed consumers | [Corrected job](https://github.com/happenings-dk/connect-cc/actions/runs/43518818289/job/132207625331) |
| Linux TSan | Initial run: all official profiles passed; interop wrapper timed out during a cold Go build; corrected run pending | [Initial job](https://github.com/happenings-dk/connect-cc/actions/runs/43515947046/job/132197812115) |
| Windows MSVC | In progress; support not yet verified | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43515947046/job/132197812179) |
| vcpkg package | Pass: installed examples and 21 public headers | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43515946715/job/132197810237) |
| Conan package | Pass: 222 tests, installed examples and public headers | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43515946715/job/132197809663) |
| Parser fuzzing | Pass: 2,770,352 inputs in 121 seconds, no finding | [Job](https://github.com/happenings-dk/connect-cc/actions/runs/43515947046/job/132197811548) |
| Benchmark harness smoke | In progress | [Run](https://github.com/happenings-dk/connect-cc/actions/runs/43515948558) |

The first macOS job failed because the runner's AppleClang 17/libc++ did not
provide `std::jthread`. One callback test now uses `std::thread`; it already
explicitly joined both threads. The corrected run used `a72576e`, also adding
dependency caching and selection of individual compiler lanes for manual runs.
Runtime, generator, protocol, packaging and example sources are identical to
`fd5f5d7`; the test's concurrency and cancellation assertions are unchanged.
The full push/PR matrix remains enabled. A macOS-only manual run intentionally
does not rerun Linux fuzzing; the separate full run above verifies it.

The first hosted TSan run passed 221 entries, including every official profile,
without a race report. `Interop.Go` reached `PASS` in its assertions but exceeded
CTest's 90-second outer budget, which also included a cold Go race compilation.
The race-enabled Go helper is now compiled during the CMake build, then executed
directly by CTest. The RPC assertions and 90-second runtime limit are unchanged.
Local generator and interoperability checks pass; the corrected hosted TSan
result must still be recorded.

Artifacts retain JUnit results, CTest logs and CMake diagnostics for 14 days.
Downloaded copies and complete job logs are preserved in the development
workspace under `build/verification/github/` and `build/verification/`.
These finite tests are compatibility evidence, not upstream endorsement or
proof that every possible input has been tested. Pending jobs do not count as
passes, and the complete objective remains open until Windows is verified.
