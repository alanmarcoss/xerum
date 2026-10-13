add_library(xerum_model STATIC
    schema.cpp
    value.cpp
    cbor.cpp
    legacy.cpp
    json.cpp
    container.cpp
    document.cpp
    decompress.cpp
    # The vendored zstd decoder and the shim that exposes it. Kept in the model library rather than a
    # separate target because nothing else in the project will ever need it.
    ${CMAKE_SOURCE_DIR}/third_party/zstd/shim.c)

target_include_directories(xerum_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(xerum_model PUBLIC cxx_std_20)

# Vendored source is not ours to lint. Our warnings inside someone else's file are noise, and noise
# is how a real warning gets ignored.
set_source_files_properties(${CMAKE_SOURCE_DIR}/third_party/zstd/shim.c
    PROPERTIES COMPILE_OPTIONS "-w")

# zlib is a system library on macOS, so the legacy container's codec costs no dependency at all.
# zstd is not, which is why third_party/zstd exists — see its README.
find_package(ZLIB REQUIRED)
target_link_libraries(xerum_model PRIVATE ZLIB::ZLIB)

# The model layer deliberately links no JUCE: it is pure and host-free, so its tests run anywhere
# (see docs/test-strategy.md). Linking JUCE here would be the first step to losing that.
set_target_properties(xerum_model PROPERTIES CXX_EXTENSIONS OFF)
