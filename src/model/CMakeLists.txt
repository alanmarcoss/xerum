add_library(xerum_model STATIC
    schema.cpp)

target_include_directories(xerum_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(xerum_model PUBLIC cxx_std_20)

# The model layer deliberately links no JUCE: it is pure and host-free, so its tests run anywhere
# (see docs/test-strategy.md). Linking JUCE here would be the first step to losing that.
set_target_properties(xerum_model PROPERTIES CXX_EXTENSIONS OFF)
