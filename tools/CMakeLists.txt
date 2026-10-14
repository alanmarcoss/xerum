# Offline authoring tools. Not product code: these run at authoring time, never in the plugin.
add_executable(xerum-wavetable-author wavetable-author.cpp)

target_link_libraries(xerum-wavetable-author PRIVATE xerum_authoring)

# The release check from ticket 16, wired as a test so "fails the build" is a fact rather than a promise:
# CI runs CTest, so a content file without a manifest entry fails the build. It skips loudly when the
# reference is not installed, like every other test that needs it (docs/test-strategy.md).
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(NAME content-provenance
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check-content-provenance.py
            --content ${CMAKE_SOURCE_DIR}/content)
