add_executable(xerum-model-tests
    schema_test.cpp
    decompress_test.cpp
    cbor_test.cpp
    container_test.cpp
    legacy_test.cpp
    document_test.cpp
    write_test.cpp
    warp_coverage_test.cpp
    filter_coverage_test.cpp)

target_link_libraries(xerum-model-tests PRIVATE
    xerum_model
    # Linked by the warp coverage test alone, and deliberately at the *test* level rather than in the
    # library: the model layer stays free of the engine (see src/model/CMakeLists.txt), and the coverage
    # test is the one place where "what the report claims we ship" and "what the engine implements" have
    # to be compared to each other.
    xerum_dsp
    Catch2::Catch2WithMain)

# Fixtures are ours, not the reference's: a payload we wrote, compressed in each of the container's
# two codecs. Committing those is what makes this suite run on a machine with no reference installed
# (see docs/test-strategy.md).
target_compile_definitions(xerum-model-tests PRIVATE
    XERUM_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

include(Catch)
catch_discover_tests(xerum-model-tests)
