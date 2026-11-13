# Sanitizer builds

Use a separate build directory with `CONNECTCPP_SANITIZE=ON` for ASan and UBSan.
Sanitizer builds default `CONNECTCPP_FETCH_GTEST=ON`: CMake downloads GoogleTest
1.18.0 with a fixed SHA-256, then builds it with the same compiler, C++20, and
sanitizer flags as the test binary. Runtime-only builds do not fetch GoogleTest.
For offline builds, provide `FETCHCONTENT_SOURCE_DIR_CONNECTCPP_GOOGLETEST`
pointing at the matching source checkout.

On this Apple Clang/libc++ installation, the prebuilt Homebrew GoogleTest archive
caused a container-overflow during `--gtest_list_tests`, inside
`testing::internal::GTestIsInitialized`, before tests ran. A standalone program
linking only GoogleTest and using `std::vector<std::string>` reproduced it. The
instrumented source build passes without disabling container checks or ASan.
The reproducer and failure traces from the investigation are in the local
`build-asan/gtest-only-repro.log` and `build-asan/startup-failure.log`.

The Homebrew Protobuf 36.1 runtime separately needs
`CONNECTCPP_PROTOBUF_GLOBALS_COMPAT=ON` when generated messages are instrumented.
See CODEGEN.md for the upstream globals-layout opt-out. A fully instrumented
Protobuf dependency build should not need that compatibility option.

These checks do not replace the remaining platform matrix or parser fuzzing.
See CONFORMANCE.md for the completed runs and outstanding release gates.

## ThreadSanitizer

Configure `CONNECTCPP_THREAD_SANITIZE=ON` in a separate build directory; it is
mutually exclusive with ASan/UBSan. GoogleTest is instrumented in this mode too.
Use `ci/triplets/x64-linux-tsan.cmake` with host triplet `x64-linux`, as in
`ci/run-sanitizer.sh thread`. The target dependency stack must also use TSan:
Protobuf's generated headers add `_tsan_detect_race` members, changing message
layouts relative to its ordinary prebuilt runtime. Mixing them produced invalid
access reports in a standalone Protobuf-only JSON/Value example, independent
of this library. No suppression or disabled TSan check is used to mask that
mismatch. The local reproducer is `build/protobuf-tsan-repro/`.
The same reproducer passes against the rebuilt dependency stack; the complete
Linux TSan suite then passes 222/222 tests, including all official profiles.
The custom compiler toolchain explicitly includes vcpkg's Linux toolchain so
the triplet's instrumentation flags reach dependencies. Merely selecting a
different triplet name without forwarding those flags does not instrument them.
The script keeps the TSan dependency installation in
`build/vcpkg-thread-installed`, separate from ordinary/ASan dependencies.
vcpkg manifest installs can remove packages no longer required by the selected
target triplet, so sharing one install tree between these configurations can
break previously configured consumers.

On the Linux verification host, Clang 18's TSan runtime also needed permission
to set `personality(ADDR_NO_RANDOMIZE)` for its own process before test discovery.
Docker's default seccomp profile denied it. The task container used the
[Moby default profile](https://github.com/moby/profiles/blob/f862455628f78f84c937e967e4a03902fb1dee11/seccomp/default.json)
with one additional allowed `personality` argument, `262144`; other filters,
dropped capabilities and no-new-privileges stayed in force. No host-wide ASLR
setting was changed. This startup problem is distinct from race-test results.

## Local static analysis

Apple Clang's analyzer reports an uninitialized coroutine-state pointer inside
Asio 1.38.2's `throw_if_cancelled` awaiter when analyzing `interceptor.cc`, as it
also does for the connection implementation. The same diagnostic reproduces in
an Asio-only coroutine, without Connect code (`build/asio-analyzer-repro.cc` and
its log); the interceptor trace is in `build/interceptor-analysis.log`. This is
recorded as an analyzer limitation, not suppressed or reported as a clean scan.
ASan/UBSan and the cancellation/deadline regressions pass independently.
