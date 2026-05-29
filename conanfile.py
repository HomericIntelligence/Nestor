from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps


class ProjectNestorConan(ConanFile):
    name = "projectnestor"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"
    # Enable OpenSSL support in cpp-httplib so SSLServer is available.
    default_options = {"cpp-httplib/*:with_openssl": True}

    def requirements(self):
        self.requires("cpp-httplib/0.18.3")
        self.requires("nlohmann_json/3.11.3")

    def build_requirements(self):
        self.test_requires("gtest/1.14.0")

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()
