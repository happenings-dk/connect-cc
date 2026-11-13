# Verification tooling

- `Dockerfile`: pinned Linux base images, Go and vcpkg registry; GCC 14 and
  Clang 18 toolchains. Build with `docker build -f ci/Dockerfile -t connectcpp-ci .`.
- `run-linux.sh gcc` / `run-linux.sh clang`: configure, build, full CTest matrix,
  install, and independent coroutine/callback/header consumers.
- `run-sanitizer.sh address` / `thread` / `fuzz`: separate instrumented builds.
  TSan uses matching instrumented target dependencies and ordinary host tools.
- `stress-timeouts.sh BUILD_DIR [REPETITIONS]`: repeated official server deadline
  cases across HTTP versions. It stops at the first failure and has no skips.
- `triplets/`: release-only Linux dependency configurations.
- `toolchains/`: compiler selection while retaining vcpkg's platform flags.
- `run-vcpkg-package.sh` and `run-conan-package.sh`: package-manager builds and
  independent consumers; Conan also reruns the full protocol matrix with its
  dependency versions. `Dockerfile.conan` adds the pinned Conan CLI to the base
  toolchain image. See [PACKAGING.md](../docs/PACKAGING.md).
- `run-benchmarks.sh NEW_OUTPUT_DIR`: optional native gRPC C++ comparison using
  a separate dependency installation. See [benchmarks](../benchmarks/README.md)
  for workload controls and interpretation limits.

Run from the source root. Scripts require the pinned checkout in `VCPKG_ROOT`.
The container runs as UID/GID 1000; its source/build mount must be writable by
that user. It needs no services, published ports or device access:

```sh
docker run --rm --cpus 4 --memory 8g --pids-limit 512 \
  --cap-drop ALL --security-opt no-new-privileges \
  --mount "type=bind,src=$PWD,dst=/workspace" \
  connectcpp-ci ci/run-linux.sh clang
```

Use separate build directories for sanitizer modes. On hosts where TSan cannot
reserve its shadow memory under ASLR, the test container may need the narrow
seccomp adjustment recorded in [SANITIZERS.md](../docs/SANITIZERS.md). Do not
change host-wide ASLR settings for these scripts. Keep the complete logs and
`Testing/Temporary/LastTest.log`; configured jobs alone do not establish a pass.

The GitHub Actions workflow is in `../.github/workflows/ci.yml`. It is prepared
for a future repository, without creating or publishing a remote repository.
