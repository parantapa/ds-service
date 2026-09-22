# type: ignore
# ConanFile declares requires and tool_requires as None,
# and Conan binds them when it loads the recipe,
# so pyright reports every call to them.
"""Conan recipe for the ds-service server."""

from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMakeDeps, CMakeToolchain, CMake


class DsServiceRecipe(ConanFile):
    """The ds-service executable as a Conan package, built with CMake."""

    name = "ds-service"
    # Set by scripts/update-version.sh, along with the other version strings.
    version = "6.1.0"

    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    exports_sources = "CMakeLists.txt", "cpp/*", "misc/*"

    def layout(self) -> None:
        cmake_layout(self)

    def requirements(self) -> None:
        self.requires("spdlog/1.17.0")
        self.requires("argparse/3.2")
        self.requires("parallel-hashmap/2.0.0")

        self.requires("grpc/1.82.0")
        self.requires("protobuf/6.33.5")
        self.requires("re2/20251105")

    def build_requirements(self) -> None:
        self.tool_requires("grpc/1.82.0")
        self.tool_requires("protobuf/6.33.5")

    def generate(self) -> None:
        deps = CMakeDeps(self)
        deps.generate()

        toolchain = CMakeToolchain(self)
        toolchain.generate()

    def build(self) -> None:
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self) -> None:
        cmake = CMake(self)
        cmake.install()

    def package_info(self) -> None:
        # This package ships an executable, not a library:
        # CMakeLists.txt installs only the ds-service target.
        # An entry for ds-service-grpc here
        # hands consumers an unresolvable -lds-service-grpc.
        self.cpp_info.libs = []
