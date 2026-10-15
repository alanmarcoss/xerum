add_library(xerum_content STATIC
    off_thread_work.cpp
    table_library.cpp)

target_include_directories(xerum_content PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xerum_content PUBLIC xerum_dsp)
target_compile_features(xerum_content PUBLIC cxx_std_20)
set_target_properties(xerum_content PROPERTIES CXX_EXTENSIONS OFF)
