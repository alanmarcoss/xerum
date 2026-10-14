# The plugin: AU (v2), VST3 and Standalone from one target, per ticket 11.
#
# `COPY_PLUGIN_AFTER_BUILD FALSE` is deliberate. Installing a plugin is a real install into the user's
# plug-in folders, and the acceptance for ticket 30 records exactly what was installed where — an
# automatic copy on every build would make that record meaningless (and would overwrite whatever the user
# already had). The AU is installed by hand for `auval`; see plugin/README.md.
#
# The AU identity is ours, not the reference's: `aumu` / `Xrm2` / `Xerm` against the reference's
# `aumu` / `Xf2X` / `Xfer`. Registering the same triple would collide on any machine with both installed.
juce_add_plugin(xerum
    PRODUCT_NAME              "xerum"
    COMPANY_NAME              "xerum"
    BUNDLE_ID                 "com.xerum.xerum"
    PLUGIN_MANUFACTURER_CODE  Xerm
    PLUGIN_CODE               Xrm2
    FORMATS                   AU VST3 Standalone
    IS_SYNTH                  TRUE
    NEEDS_MIDI_INPUT          TRUE
    AU_MAIN_TYPE              kAudioUnitType_MusicDevice
    VST3_CATEGORIES           Instrument Synth
    DESCRIPTION               "xerum — an independent wavetable synthesiser (Serum 2 preset compatible)"
    COPY_PLUGIN_AFTER_BUILD   FALSE)

# Absolute, because the test target in `tests/` compiles the same translation units and a relative list
# would be resolved against *that* directory.
set(XERUM_PLUGIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/ParameterBridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/PresetState.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/XerumAudioProcessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/XerumAudioProcessorEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/InterfacePlan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/ParameterPanels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/ImportReportView.cpp)

target_sources(xerum PRIVATE ${XERUM_PLUGIN_SOURCES})

target_compile_definitions(xerum PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    # JUCE's VST3 wrapper synthesises 2048 "MIDI CC c|n" parameters plus channel-expression
    # parameters for an instrument (default 1, `juce_audio_plugin_client_VST3.cpp`). They are not
    # xerum's identity and they are not the reference's either: measured through the harness, they took
    # the exposed surface from 59 parameters to 2140. With it off, MIDI CCs still arrive as MIDI events
    # in `processBlock` — which is where xerum reads them — and the surface a host shows is xerum's own.
    JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0
    # The source tree's `content/`, so the standalone and the tests find xerum's own tables without an
    # install step. It is the last root tried and it simply will not exist on a user's machine; see
    # `XerumAudioProcessor::contentRoots`.
    XERUM_CONTENT_DIR="${CMAKE_SOURCE_DIR}/content")

# `xerum_model` is pure C++ (no JUCE) and `xerum_dsp`/`xerum_content` likewise; the plugin is the only
# place the two worlds meet, which is what keeps the format and DSP suites runnable without a host.
target_link_libraries(xerum PRIVATE
    xerum_model
    xerum_dsp
    xerum_content
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)

add_subdirectory(tests)
