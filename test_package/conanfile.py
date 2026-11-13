"""Independent CMake consumer for the connectcpp Conan package."""

from __future__ import annotations

import os

import conan
from conan.tools import build as build_tools
from conan.tools import cmake


class ConnectCppPackageTest(conan.ConanFile):
    """Compiles and runs a consumer using only packaged artifacts."""

    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeDeps", "CMakeToolchain", "VirtualRunEnv"
    test_type = "explicit"

    def requirements(self) -> None:
        """Tests the exact package revision produced by conan create."""
        self.requires(self.tested_reference_str)

    def layout(self) -> None:
        """Uses standard Conan CMake build directories."""
        cmake.cmake_layout(self)

    def build(self) -> None:
        """Builds without source-tree include or library paths."""
        builder = cmake.CMake(self)
        builder.configure()
        builder.build()

    def test(self) -> None:
        """Executes the consumer when the target can run on the build host."""
        if build_tools.can_run(self):
            executable = os.path.join(self.cpp.build.bindir, "consumer")
            self.run(executable, env="conanrun")
