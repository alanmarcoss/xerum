add_library(xerum_dsp STATIC
    wavetable.cpp
    oscillator.cpp
    warp.cpp
    filter.cpp
    modulation.cpp
    voice.cpp
    fx_rack.cpp
    spectral_morph.cpp)

# **Authoring-only code, deliberately not in `xerum_dsp`.**
#
# `docs/content-policy.md` says the formula evaluator is an offline authoring tool and not a plugin
# feature. Leaving it inside the engine library made that a hope about which objects the linker happens to
# pull in — true today, because nothing on the audio path references it, but a policy that depends on
# linker behaviour is not a policy. A separate library makes it a fact of the link graph: the plugin links
# `xerum_dsp` and cannot reach this.
#
# Only two things link it, and both are off the plugin path: the offline tool (`tools/`) and the tests
# that verify it.
add_library(xerum_authoring STATIC
    wavetable_write.cpp)

target_link_libraries(xerum_authoring PUBLIC xerum_dsp)
target_include_directories(xerum_authoring PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(xerum_authoring PUBLIC cxx_std_20)

target_include_directories(xerum_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(xerum_dsp PUBLIC cxx_std_20)

# Like the model layer, the DSP layer links no JUCE and touches no filesystem: it is pure, so its tests
# run anywhere and its behaviour is reproducible from bytes (see docs/test-strategy.md).
set_target_properties(xerum_dsp PROPERTIES CXX_EXTENSIONS OFF)
