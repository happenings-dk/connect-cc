# Conan support

The recipe is the conventional root `conanfile.py`; `test_package/` is its
independent consumer. `connectcpp-cxx20.cmake` preserves the C++20 requirement
when Conan's CMakeDeps generates the imported target. Runtime dependencies and
native build tools are separate so protoc can run while cross-compiling.
