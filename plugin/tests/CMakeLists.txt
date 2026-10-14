# The plugin's own suite. It links the plugin's translation units directly rather than through the
# `juce_add_plugin` target, so no wrapper is linked and no host is needed: the tests drive
# `XerumAudioProcessor` in-process, which is what makes them runnable anywhere.
#
# Run alone:
#   ctest --test-dir build-30 -R xerum-plugin-tests
#   build-30/plugin/tests/xerum-plugin-tests "[plugin]"
#
# The plugin layer's *validation* is a separate concern and a separate tool: `pluginval` and `auval`,
# recorded in plugin/README.md.

add_executable(xerum-plugin-tests
    AllocationCounter.cpp
    bridge_test.cpp
    state_test.cpp
    audio_test.cpp
    preset_test.cpp
    realtime_test.cpp
    interface_test.cpp)

target_sources(xerum-plugin-tests PRIVATE ${XERUM_PLUGIN_SOURCES})

target_include_directories(xerum-plugin-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source
    ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(xerum-plugin-tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0
    XERUM_CONTENT_DIR="${CMAKE_SOURCE_DIR}/content")

target_link_libraries(xerum-plugin-tests PRIVATE
    xerum_model
    xerum_dsp
    xerum_content
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(xerum-plugin-tests)
