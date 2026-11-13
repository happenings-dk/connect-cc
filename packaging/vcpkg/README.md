# vcpkg local package

Prepare a self-contained overlay from the repository root:

```sh
cmake -DOUTPUT_ROOT="$PWD/build/vcpkg-overlay" \
  -P packaging/vcpkg/prepare.cmake
```

Use its `connectcpp` subdirectory with `--overlay-ports` or `VCPKG_OVERLAY_PORTS`, and use the
registry baseline from the root `vcpkg.json`. Each snapshot requires a fresh
output directory. The source is deliberately inside the port so binary-cache
keys include changes to runtime files, even before a source repository or
release archive exists.
The preparation command also writes a small consumer manifest in `manifest/`;
use manifest mode so a changed port hash rebuilds an already-installed package.
`ci/run-vcpkg-package.sh [ABSOLUTE_OVERLAY_DIRECTORY]` verifies the Linux package
and all standalone examples using that manifest.

The package includes the project's Apache-2.0 license. This is a local
development recipe, not a registry submission. Registry publication is a
separate follow-up action in the accepted plan.
