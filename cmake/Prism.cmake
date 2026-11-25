# Prism (https://github.com/ethindp/prism, MPL-2.0): screen reader and
# text-to-speech abstraction used for accessibility speech output.
#
# The prebuilt release package for the target platform is downloaded at
# configure time. Prism needs a C++23 compiler to build from source, so the
# prebuilt binaries keep this project's own toolchain requirements unchanged.
#
# Options:
#   BM64_ENABLE_SPEECH   Build with speech support (default ON). When OFF,
#                        the speech API still exists but does nothing.
#   PRISM_PACKAGE_DIR    Use an already-extracted Prism release package
#                        (e.g. for offline or Flatpak builds) instead of
#                        downloading one.

option(BM64_ENABLE_SPEECH "Enable screen reader / text-to-speech output via Prism" ON)
set(PRISM_PACKAGE_DIR "" CACHE PATH "Path to an extracted Prism release package (skips the download)")

if (NOT BM64_ENABLE_SPEECH)
    message(STATUS "Prism speech: disabled")
    return()
endif()

set(PRISM_VERSION "0.18.4")

if (NOT PRISM_PACKAGE_DIR)
    if (WIN32)
        if (CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            set(_prism_asset "windows-arm64")
            set(_prism_sha256 "174693d98330600cfdd0b67fea84846c3bee93ad48b563c1e1ae6104d47ca0c9")
        else()
            set(_prism_asset "windows-x64")
            set(_prism_sha256 "623ab4d0efb00d0cd278ed9e6c79c96b60951d285bf2ba8524ce4b718d003520")
        endif()
    elseif (APPLE)
        set(_prism_asset "macos-universal")
        set(_prism_sha256 "838195f8e671b85fcea5c7239e27dd15a30e2cac1e320df4288e16a3c49391be")
    elseif (CMAKE_SYSTEM_NAME MATCHES "Linux")
        if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
            set(_prism_asset "linux-arm64")
            set(_prism_sha256 "29dec5692d0f7b3a95fa76da9cc92bfdc73698d0706c31d740b204967da925af")
        else()
            set(_prism_asset "linux-x64")
            set(_prism_sha256 "a779b010d291f6148a2b99d4459fb92547a1715922296b4b86ee135318d1356d")
        endif()
    else()
        message(FATAL_ERROR "Prism speech: no prebuilt package for ${CMAKE_SYSTEM_NAME}. "
                            "Set PRISM_PACKAGE_DIR or configure with -DBM64_ENABLE_SPEECH=OFF.")
    endif()

    include(FetchContent)
    FetchContent_Declare(
        prism
        URL https://github.com/ethindp/prism/releases/download/v${PRISM_VERSION}/prism-${_prism_asset}.zip
        URL_HASH SHA256=${_prism_sha256}
    )
    FetchContent_MakeAvailable(prism)
    set(PRISM_PACKAGE_DIR "${prism_SOURCE_DIR}")
endif()

# Use the shared-library build; it exports prism::prism backed by prism.dll /
# libprism.so / libprism.dylib.
find_package(prism CONFIG REQUIRED PATHS "${PRISM_PACKAGE_DIR}/dynamic/release" NO_DEFAULT_PATH)
message(STATUS "Prism speech: using ${PRISM_PACKAGE_DIR}")

target_link_libraries(BM64Recompiled PRIVATE prism::prism)
target_compile_definitions(BM64Recompiled PRIVATE BM64_HAS_PRISM)

# Ship the Prism shared library next to the executable.
add_custom_command(TARGET BM64Recompiled POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:prism::_prism_lib>
        $<TARGET_FILE_DIR:BM64Recompiled>)

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    # Find libprism.so beside the executable at runtime.
    set_property(TARGET BM64Recompiled APPEND PROPERTY BUILD_RPATH "$ORIGIN")
    set_property(TARGET BM64Recompiled APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
endif()
