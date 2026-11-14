# Linux dependency triplets

`x64-linux.cmake` builds release-only static libraries with the dynamic C runtime.
`x64-linux-tsan.cmake` uses Clang 18 and instruments target dependencies with
ThreadSanitizer. Pair it with the ordinary `x64-linux` host triplet so build
tools remain usable independently of sanitizer runtime startup requirements.

`x64-linux-asan.cmake` instruments target dependencies with ASan/UBSan and also
uses the ordinary host triplet. Protobuf container annotations must agree
between generated code and the runtime: an uninstrumented parser can populate
a repeated field without unpoisoning its inline storage. Do not disable
container-overflow detection to hide a mixed dependency configuration.

Only OpenSSL uses `-DPEDANTIC -fno-sanitize-recover=all
-fno-sanitize=function`, matching its upstream Clang UBSan configuration for
generic C callbacks. ASan and the remaining UBSan checks stay enabled there;
connect-cc and other dependencies retain function-type checks. See
[the sanitizer notes](../../docs/SANITIZERS.md) for sources and limits.
