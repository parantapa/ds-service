# type: ignore
# ConanFile declares requires and tool_requires as None,
# and Conan binds them when it loads the recipe,
# so pyright reports every call to them.
"""Conan recipe for the ds-service server and its C++ client."""

from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMakeDeps, CMakeToolchain, CMake


class DsServiceRecipe(ConanFile):
    """The ds-service executable as a Conan package, built with CMake.

    with_server, with_client and with_python select what the build produces.
    """

    name = "ds-service"
    # Set by scripts/update-version.sh, along with the other version strings.
    version = "7.0.0"

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_server": [True, False],
        "with_client": [True, False],
        "with_python": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_server": True,
        "with_client": True,
        "with_python": False,
    }

    exports_sources = "CMakeLists.txt", "cmake/*", "cpp/*"

    def layout(self) -> None:
        cmake_layout(self)

    def requirements(self) -> None:
        if self.options.with_server:
            self.requires("spdlog/1.17.0")
            self.requires("argparse/3.2")
            self.requires("parallel-hashmap/2.0.0")
            self.requires("re2/20251105")

        self.requires("grpc/1.82.0")
        self.requires("protobuf/6.33.5")

    def build_requirements(self) -> None:
        # protoc and grpc_cpp_plugin generate code from the proto at build time.
        # The versions match requirements(),
        # because generated protobuf code only works with the runtime it came from.
        self.tool_requires("grpc/1.82.0")
        self.tool_requires("protobuf/6.33.5")

    def generate(self) -> None:
        deps = CMakeDeps(self)
        deps.generate()

        toolchain = CMakeToolchain(self)
        toolchain.variables["DS_SERVICE_BUILD_SERVER"] = bool(self.options.with_server)
        toolchain.variables["DS_SERVICE_BUILD_CLIENT"] = bool(self.options.with_client)
        toolchain.variables["DS_SERVICE_BUILD_PYTHON"] = bool(self.options.with_python)
        toolchain.generate()

    def build(self) -> None:
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self) -> None:
        cmake = CMake(self)
        cmake.install()

    def package_info(self) -> None:
        # This package ships no C++ library for a consumer to link.
        # CMakeLists.txt installs the ds-service executable with with_server,
        # and the _ext module and its stub with with_python.
        self.cpp_info.libs = []
