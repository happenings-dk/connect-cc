# Linux dependency triplets

`x64-linux.cmake` builds release-only static libraries with the dynamic C runtime.
`x64-linux-tsan.cmake` uses Clang 18 and instruments target dependencies with
ThreadSanitizer. Pair it with the ordinary `x64-linux` host triplet so build
tools remain usable independently of sanitizer runtime startup requirements.
