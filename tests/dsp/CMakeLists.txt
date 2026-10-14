add_executable(xerum-dsp-tests
    wavetable_test.cpp
    warp_test.cpp)

target_link_libraries(xerum-dsp-tests PRIVATE
    xerum_dsp
    Catch2::Catch2WithMain)

target_compile_definitions(xerum-dsp-tests PRIVATE
    XERUM_DSP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

include(Catch)
catch_discover_tests(xerum-dsp-tests)
