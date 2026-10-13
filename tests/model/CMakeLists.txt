add_executable(xerum-model-tests
    schema_test.cpp)

target_link_libraries(xerum-model-tests PRIVATE
    xerum_model
    Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(xerum-model-tests)
