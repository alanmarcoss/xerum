add_executable(xerum-model-tests
    schema_test.cpp
    decompress_test.cpp
    cbor_test.cpp
    container_test.cpp
    legacy_test.cpp
    document_test.cpp
    write_test.cpp
    warp_coverage_test.cpp
    filter_coverage_test.cpp
    fx_topology_test.cpp
    reference_roundtrip_test.cpp
    modulation_coverage_test.cpp
    defaults_test.cpp
    engine_selection_test.cpp
    structural_fidelity_test.cpp
    third_party_pack_test.cpp
    noise_sub_vocabulary_test.cpp)

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
    XERUM_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    # The build tree this binary was built in. The round-trip test needs it to find the *harness it was
    # built alongside* rather than whichever one happens to sit at the shortest path — see
    # `findHostBinary` in reference_roundtrip_test.cpp for why that distinction is load-bearing.
    XERUM_BUILD_DIR="${CMAKE_BINARY_DIR}")

include(Catch)
# The reference's content tree is a **shared, mutable** resource: the round-trip cases install a preset
# into `Presets/User/Default.SerumPreset` to drive the plugin, so two of them running concurrently
# corrupt each other's fixture — measured, as 5 failures under `ctest -j4` that all pass serially.
# A resource lock serialises exactly the tests that touch it, and states the reason here rather than
# relying on everyone remembering not to run the suite in parallel.
catch_discover_tests(xerum-model-tests PROPERTIES RESOURCE_LOCK reference_content_root)
