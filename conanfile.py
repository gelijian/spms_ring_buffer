from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout


class SpmsRingBufferConan(ConanFile):
    name = "spms_ring_buffer"
    version = "1.0.0"
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = (
        "*.h",
        "*.cc",
        "CMakeLists.txt",
        ".clang-format",
        "tests/CMakeLists.txt",
        "tests/spms_ring_buffer_test.cc",
    )

    def requirements(self):
        self.requires("doctest/2.4.11")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self, cmake):
        cmake.configure()
        cmake.build()

    def package_info(self):
        self.cpp_info.libs = []
