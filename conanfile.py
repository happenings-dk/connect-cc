"""Conan 2 recipe for the experimental connectcpp static runtime."""

from __future__ import annotations

import os

import conan
from conan.tools import build as build_tools
from conan.tools import cmake
from conan.tools import files
from conan.tools import gnu


class ConnectCppConan(conan.ConanFile):
    """Builds the C++ runtime with a matching host protobuf compiler."""

    name = "connectcpp"
    version = "0.1.0"
    description = "Experimental C++20 ConnectRPC client and server"
    license = "Apache-2.0"
    package_type = "static-library"
    settings = "os", "arch", "compiler", "build_type"
    options = {"fPIC": [True, False], "protobuf_lite": [True, False]}
    default_options = {"fPIC": True, "protobuf_lite": False}
    exports_sources = (
        "CMakeLists.txt", "cmake/*", "include/*", "src/*", "proto/*",
        "packaging/conan/*", "README.md", "LICENSE",
    )
    required_conan_version = ">=2.34.0 <3"

    def config_options(self) -> None:
        """Omits position-independent-code configuration on Windows."""
        # Conan replaces declarative settings with a Settings object at load.
        if self.settings.os == "Windows":  # pylint: disable=no-member
            self.options.rm_safe("fPIC")

    def requirements(self) -> None:
        """Pins runtime dependencies and exposes public header requirements."""
        self.requires("protobuf/7.36.2", transitive_headers=True,
                      transitive_libs=True,
                      options={"lite": bool(self.options.protobuf_lite)})
        self.requires("asio/1.38.2", transitive_headers=True)
        self.requires("openssl/3.6.5", transitive_headers=True,
                      transitive_libs=True)
        self.requires("abseil/20260107.1")
        self.requires("llhttp/9.3.0")
        self.requires("libnghttp2/1.68.1")
        self.requires("zlib/1.4")

    def build_requirements(self) -> None:
        """Uses native build tools, including when the target is different."""
        # Conan replaces this declarative attribute with a callable in recipes.
        self.tool_requires("protobuf/<host_version>")  # pylint: disable=not-callable
        self.tool_requires("pkgconf/2.5.1")  # pylint: disable=not-callable

    def validate(self) -> None:
        """Requires the public API's C++20 language level."""
        build_tools.check_min_cppstd(self, "20")

    def layout(self) -> None:
        """Separates CMake sources, generated files and build output."""
        cmake.cmake_layout(self)

    def generate(self) -> None:
        """Provides CMake and pkg-config dependency metadata."""
        cmake.CMakeDeps(self).generate()
        gnu.PkgConfigDeps(self).generate()
        toolchain = cmake.CMakeToolchain(self)
        toolchain.variables["BUILD_TESTING"] = False
        toolchain.variables["BUILD_SHARED_LIBS"] = False
        toolchain.variables["CONNECTCPP_BUILD_GENERATOR"] = False
        toolchain.variables["CONNECTCPP_BUILD_CONFORMANCE"] = False
        toolchain.variables["CONNECTCPP_PROTOBUF_LITE"] = bool(
            self.options.protobuf_lite
        )
        toolchain.variables["ASIO_INCLUDE_DIR"] = (
            self.dependencies["asio"].cpp_info.includedirs[0]
        )
        # The build-context settings are injected by Conan before generation.
        build_os = self.settings_build.os  # pylint: disable=no-member
        protoc_name = "protoc.exe" if build_os == "Windows" else "protoc"
        toolchain.variables["Protobuf_PROTOC_EXECUTABLE"] = os.path.join(
            self.dependencies.build["protobuf"].cpp_info.bindirs[0], protoc_name
        )
        toolchain.generate()

    def build(self) -> None:
        """Builds the static runtime and its private protobuf messages."""
        builder = cmake.CMake(self)
        builder.configure()
        builder.build()

    def package(self) -> None:
        """Installs headers, library, CMake exports and Conan metadata."""
        cmake.CMake(self).install()
        files.copy(self, "LICENSE", src=self.source_folder,
                   dst=os.path.join(self.package_folder, "licenses"))
        files.copy(
            self, "connectcpp-cxx20.cmake",
            src=os.path.join(self.source_folder, "packaging", "conan"),
            dst=os.path.join(self.package_folder, "lib", "cmake", "connectcpp"),
        )

    def package_info(self) -> None:
        """Uses the same CMake target name as the native installation."""
        self.cpp_info.libs = ["connectcpp"]
        self.cpp_info.defines = ["ASIO_STANDALONE"]
        protobuf_component = "libprotobuf"
        if self.options.protobuf_lite:
            protobuf_component = "libprotobuf-lite"
            self.cpp_info.defines.append("CONNECTCPP_PROTOBUF_LITE=1")
        self.cpp_info.requires = [
            f"protobuf::{protobuf_component}", "asio::asio", "openssl::ssl",
            "abseil::absl_strings", "llhttp::llhttp", "libnghttp2::nghttp2",
            "zlib::zlib",
        ]
        if self.settings.os == "Macos":  # pylint: disable=no-member
            self.cpp_info.frameworks = ["Security", "CoreFoundation"]
        elif self.settings.os == "Windows":  # pylint: disable=no-member
            self.cpp_info.system_libs = ["crypt32"]
        self.cpp_info.set_property("cmake_file_name", "connectcpp")
        self.cpp_info.set_property(
            "cmake_target_name", "connectcpp::connectcpp"
        )
        self.cpp_info.set_property(
            "cmake_build_modules",
            ["lib/cmake/connectcpp/connectcpp-cxx20.cmake"],
        )
