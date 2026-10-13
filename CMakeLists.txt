cmake_minimum_required(VERSION 3.22)

project(xerum VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "arm64 only, per the map")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# JUCE is pinned to the exact commit of the 9.0.4 tag rather than the tag name, so a moved
# tag cannot silently change what we build. Licence: JUCE 9 is AGPLv3 or commercial; xerum
# uses it under AGPLv3 (see .scratch/xerum/issues/11-stack-and-licence.md).
include(FetchContent)

# Register CTest so `ctest --test-dir build` works from the start. The suites arrive with the
# format and DSP layers; until then this exists so CI can *discover* suites rather than
# hard-coding target names.
enable_testing()

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        3fb5cbc0340758596dac39f7c8de47d562b4896e  # 9.0.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# The host harness (ticket 08). Layout is provisional until ticket 13 settles the architecture.
add_subdirectory(host)
