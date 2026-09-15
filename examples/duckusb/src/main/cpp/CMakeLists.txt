cmake_minimum_required(VERSION 3.22.1)

# Ported from DuckUSB's Android.mk / Application.mk (ndkBuild). The module is built with CMake
# like every other native component in this repository, because the root build.gradle.kts
# injects the NDK version, the arm64-v8a-only abiFilter and the 16 KB page-size linker flag
# through the CMake arguments; an Android.mk would silently opt out of all three.
#
# APP_STL := c++_static upstream maps to the NDK's default (c++_static) here; the module is
# linked without an explicit ANDROID_STL, so libc++ is linked statically.
project(vector_duckusb LANGUAGES CXX)

add_library(vector_duckusb SHARED native_hooks.cpp)

target_compile_features(vector_duckusb PRIVATE cxx_std_17)

# liblog for __android_log_print; libdl is part of bionic, and dl_iterate_phdr (used to find
# libart's dynamic symbol table) comes from libc.
target_link_libraries(vector_duckusb PRIVATE log dl)
