add_library(xerum_dsp STATIC
    wavetable.cpp
    oscillator.cpp)

target_include_directories(xerum_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(xerum_dsp PUBLIC cxx_std_20)

# Like the model layer, the DSP layer links no JUCE and touches no filesystem: it is pure, so its tests
# run anywhere and its behaviour is reproducible from bytes (see docs/test-strategy.md).
set_target_properties(xerum_dsp PROPERTIES CXX_EXTENSIONS OFF)
