add_executable(xerum-dsp-tests
    wavetable_test.cpp
    warp_test.cpp
    filter_test.cpp
    modulation_test.cpp
    voice_test.cpp
    fx_rack_test.cpp
    wavetable_write_test.cpp
    spectral_morph_test.cpp)

target_link_libraries(xerum-dsp-tests PRIVATE
    xerum_dsp
    # Test-only, for the wavetable-write test: it proves a missing table is still *reported* through the
    # real TableLibrary rather than silently falling back. `xerum_dsp` itself stays free of the content
    # layer — the link is here, not in the library.
    xerum_content
    # Ticket 34's writer is authoring-only code in its own library, so the test that verifies it links it
    # here rather than it being part of the engine. Same reasoning as `xerum_content` above: the link
    # belongs to the test, not to the library.
    xerum_authoring
    Catch2::Catch2WithMain)

target_compile_definitions(xerum-dsp-tests PRIVATE
    XERUM_DSP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

include(Catch)
catch_discover_tests(xerum-dsp-tests)
