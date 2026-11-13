# Linux GCC 14 benchmark evidence

Recorded 13 November 2026 from the compression-preference and gzip-member implementation.
`linux-gcc14.csv` contains all 90 measured rows, including cross-implementation
calls. Every row completed its expected request count with zero errors.

Companion text files record the machine and CPU quota, compiler and CMake,
pinned package graph, source and executable SHA-256 hashes, and CPU accounting
before and after measurement. The CPU counters include the same container's
build activity before the first snapshot; compare their deltas, not absolute
values. The measurement interval added no throttled periods or throttled time.

The full CMake cache and execution logs are retained under
`build/verification/release-audit/build/benchmark-results-audit/` and
`build/verification/release-audit/build/benchmark-audit.log` in the development workspace.
See [BENCHMARKS.md](../BENCHMARKS.md) for the summary and reproduction commands.

An exact benchmark source archive and its SHA-256 are retained beside those
logs as `benchmark-audit-source.tgz` and `benchmark-audit-source-sha256.txt`.
The source fingerprint matches the current local benchmark/runtime files.
