# The host harness: load a plugin, drive it, render audio, write a WAV.
#
# It serves three masters, which is why it is the first thing built:
#   * ticket 08 — render a preset through the real Serum 2 headlessly
#   * ticket 09 — capture golden reference renders
#   * ticket 07 — give REA something to launch so the plugin's preset reads can be observed
#
# Console, no GUI. A JUCE console app still links the GUI modules because
# juce_audio_processors depends on them, and because some hosted plugins expect a
# message manager to exist.

juce_add_console_app(xerum-host
    PRODUCT_NAME "xerum-host"
    COMPANY_NAME "xerum")

target_sources(xerum-host PRIVATE main.cpp)

target_compile_definitions(xerum-host PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

# juce_audio_processors_headless depends only on juce_audio_basics and juce_events, so the
# harness builds without any GUI module. addHeadlessDefaultFormatsToManager() is the entry point.
target_link_libraries(xerum-host PRIVATE
    juce::juce_audio_processors_headless
    juce::juce_audio_formats
    juce::juce_events
    juce::juce_recommended_config_flags)
