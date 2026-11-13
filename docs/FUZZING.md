# Protocol fuzzing

`connectcpp_protocol_fuzz` drives bounded envelope and streaming parsers,
HTTP/1.1 requests/responses and pipelining, gRPC-Web base64/trailers, Connect
terminal/error JSON, GET decoding, gRPC timeout/status/details, content types,
Connect compression preferences, and unary identity/gzip responses. The first two input bytes select the parser,
protocol/flags and fragment size. Seeds in `fuzz/corpus` include valid frames,
terminal records, HTTP messages, metadata, GET parameters and concatenated gzip
members (including empty members).

Build with upstream Clang and its libFuzzer runtime. Enable ASan/UBSan too;
`fuzzer-no-link` instruments the library itself, not just the entry point.
For a prebuilt protobuf runtime, use the compatibility option described in
SANITIZERS.md when required by that runtime version.

```sh
cmake -S . -B build-fuzz -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=OFF -DCONNECTCPP_BUILD_GENERATOR=OFF \
  -DCONNECTCPP_BUILD_FUZZERS=ON -DCONNECTCPP_SANITIZE=ON \
  -DCONNECTCPP_PROTOBUF_GLOBALS_COMPAT=ON
cmake --build build-fuzz --parallel 4
mkdir -p build-fuzz/corpus build-fuzz/artifacts
cp fuzz/corpus/* build-fuzz/corpus/
build-fuzz/connectcpp_protocol_fuzz build-fuzz/corpus \
  -max_total_time=120 -max_len=4096 -timeout=10 -rss_limit_mb=1024 \
  -artifact_prefix=build-fuzz/artifacts/
```

Add the vcpkg toolchain flags from DEPENDENCIES.md for the pinned dependency
baseline. CI retains the evolved corpus, run log, and any failure artifacts.
Reproduce a failure by passing its artifact file as the sole positional input.
Promote reproducible failures to regression tests before fixing them. Longer
campaigns can increase the time/input-length limits; the entry point rejects
inputs above 64 KiB and keeps message/decompression/metadata limits at 64 KiB.
The harness also aborts if a parser produces events indefinitely without
consuming input. This is a liveness check, not a throughput benchmark.

Fuzz configuration is separate from TSan. The CI smoke budget is finite and is
not a claim of exhaustive parser coverage or proof of protocol compliance.
CONFORMANCE.md tracks executed verification, distinct from configured jobs.

After adding concatenated gzip decoding and its seed, Linux Clang 18 with
ASan/UBSan completed 7,065,302 inputs in 121 seconds without a finding. This
task run used a two-CPU, 8-GiB container and the existing evolved corpus with
the new seed added. The log and retained corpus live in the verification
workspace; the source seed is `fuzz/corpus/gzip-members`. This result supersedes
the earlier campaign as evidence for the current decoder, without implying
exhaustive coverage.

After correcting Connect's ordered compression preferences, a fresh campaign
completed 5,798,617 inputs in 121 seconds without a finding, under the same
Linux Clang 18 ASan/UBSan limits. Mode 6 now also feeds arbitrary encoding lists
to the Connect streaming request decoder; `fuzz/corpus/encoding-preference`
seeds an identity-before-gzip list. The latest log and evolved corpus are
retained under `build/verification/release-audit/` in the development workspace.
This is the latest campaign against the current runtime.
