add_executable(xerum-content-tests
    table_library_test.cpp)

target_link_libraries(xerum-content-tests PRIVATE
    xerum_content
    Catch2::Catch2WithMain)

target_compile_definitions(xerum-content-tests PRIVATE
    XERUM_CONTENT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

include(Catch)
catch_discover_tests(xerum-content-tests)
