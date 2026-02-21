# type: ignore
from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMakeDeps, CMakeToolchain, CMake


class DsServiceRecipe(ConanFile):
    settings = "os", "compiler", "build_type", "arch"

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        self.requires("spdlog/1.17.0")
        self.requires("argparse/3.2")
        self.requires("parallel-hashmap/2.0.0")
        self.requires("protobuf/5.27.0")
        self.requires("grpc/1.72.0")

    def build_requirements(self):
        self.requires("grpc/1.72.0", build=True)
        self.requires("protobuf/5.27.0", build=True)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()

        toolchain = CMakeToolchain(self)
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
