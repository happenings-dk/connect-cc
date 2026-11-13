# Package recipes

- `vcpkg/`: static-runtime overlay template and source-snapshot preparation.
- `conan/`: Conan support files; the recipe is the root `conanfile.py`.

Both recipes package the runtime independently of the Go generator. Build the
matching generator from the same source snapshot using the commands in
[CODEGEN.md](../docs/CODEGEN.md). Package-manager metadata and source snapshots
are local artifacts; no registry or repository publication is performed.
Both recipes include the root Apache-2.0 `LICENSE`.
