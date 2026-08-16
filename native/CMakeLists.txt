cmake_minimum_required(VERSION 3.10)

project(native)

set(CMAKE_CXX_STANDARD 23)

add_subdirectory(${VECTOR_ROOT}/external external)

file(GLOB_RECURSE NATIVE_SOURCES "src/*.cpp")
set(ART_HOOK_SOURCES)
if (CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a" OR
    CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    file(GLOB ART_HOOK_SOURCES "src/art_hook/*.c")
    list(APPEND ART_HOOK_SOURCES "src/art_hook/hook_engine_oat_patch.c")
    list(REMOVE_DUPLICATES ART_HOOK_SOURCES)
endif()
add_library(${PROJECT_NAME} STATIC ${NATIVE_SOURCES} ${ART_HOOK_SOURCES})

set(IGNORED_WARNINGS
    -Wno-c99-extensions
    -Wno-extra-semi
    -Wno-gnu-flexible-array-initializer
    -Wno-gnu-string-literal-operator-template
    -Wno-gnu-zero-variadic-macro-arguments
    -Wno-variadic-macros
    -Wno-zero-length-array
)

# --- Include Directories ---
# Add the 'include' directory as a PUBLIC include path. This means any
# target that links against vector_native will also inherit this include path.
target_include_directories(${PROJECT_NAME} PUBLIC include)

# Add the 'src' directory and external XZ library includes as PRIVATE.
# These are only needed to build vector_native itself.
target_include_directories(${PROJECT_NAME} PRIVATE
    src
    src/art_hook
    ${VECTOR_ROOT}/external/xz-embedded/linux/include
)

# The imported rustFrida engine is an ARM64-only backend.  Keep the C sources in
# the target so Android's ABI-specific compiler can exclude them cleanly on any
# future non-ARM64 build.
target_compile_definitions(${PROJECT_NAME} PRIVATE VECTOR_WXSHADOW_ART_HOOK=1)

# --- Compiler Options ---
# Apply the standard pedantic warnings and the ignored warnings list.
target_compile_options(${PROJECT_NAME} PRIVATE -Wpedantic ${IGNORED_WARNINGS})

# --- Library Linking ---
# Link against required external and system libraries.
# PUBLIC libraries are propagated to targets that link against this one.
# PRIVATE libraries are only used for the compilation of this target.
target_link_libraries(${PROJECT_NAME} PUBLIC
    dobby_static
    lsplant_static
    xz_static
    log # Android logging library
    fmt-header-only
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    dex_builder_static
)
