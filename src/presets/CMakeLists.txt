add_library(xerum_presets STATIC
    factory_bank.cpp)

target_include_directories(xerum_presets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The bank is authored against the engine's own vocabulary — a warp mode, a filter model, a noise
# colour, a matrix source — so it links the engine rather than restating its names. That is what lets
# the authoring tool refuse a table, warp or filter the engine cannot build *before* a preset is
# written, instead of shipping a preset whose parameter resolves to "not in xerum's set".
target_link_libraries(xerum_presets PUBLIC xerum_model xerum_dsp)
target_compile_features(xerum_presets PUBLIC cxx_std_20)
set_target_properties(xerum_presets PROPERTIES CXX_EXTENSIONS OFF)
