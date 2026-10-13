add_executable(xerum-model-tests
    schema_test.cpp
    decompress_test.cpp)

target_link_libraries(xerum-model-tests PRIVATE
    xerum_model
    Catch2::Catch2WithMain)

# Fixtures are ours, not the reference's: a payload we wrote, compressed in each of the container's
# two codecs. Committing those is what makes this suite run on a machine with no reference installed
# (see docs/test-strategy.md).
target_compile_definitions(xerum-model-tests PRIVATE
    XERUM_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

include(Catch)
catch_discover_tests(xerum-model-tests)
