cmake_minimum_required(VERSION 3.22.1)

project(vector_nathook_demo LANGUAGES CXX)

add_library(vector_nathook_demo SHARED native_hook_demo.cpp)

target_compile_features(vector_nathook_demo PRIVATE cxx_std_17)

# __android_log_print -> liblog; dlsym/dlerror -> libdl. pthread is part of bionic libc on Android.
target_link_libraries(vector_nathook_demo PRIVATE log dl)