# type: ignore
from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMakeDeps, CMakeToolchain, CMake


class DsServiceRecipe(ConanFile):
    name = "ds-service"
    version = "main"

    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    exports_sources = "CMakeLists.txt", "cpp/*", "misc/*"

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        self.requires("spdlog/1.17.0")
        self.requires("argparse/3.2")
        self.requires("parallel-hashmap/2.0.0")

        self.requires("grpc/1.78.1")
        self.requires("protobuf/6.33.5")

    def build_requirements(self):
        self.tool_requires("grpc/1.78.1")
        self.tool_requires("protobuf/6.33.5")
        self.tool_requires("cmake/4.3.0")

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()

        toolchain = CMakeToolchain(self)
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["ds-service-grpc"]
