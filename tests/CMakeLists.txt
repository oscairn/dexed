# Render tests. See docs/oscairn-plan.md, section 4.11.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEXED_BASELINE ${CMAKE_CURRENT_SOURCE_DIR}/baseline/dx7-renders.txt)

# The plugin, as a black box: links the shared code like JUCE's own wrapper targets do.
add_executable(dexed-render-plugin render_plugin.cpp)
target_include_directories(dexed-render-plugin PRIVATE $<TARGET_PROPERTY:Dexed,INCLUDE_DIRECTORIES>)
target_link_libraries(dexed-render-plugin PRIVATE Dexed)
# The processor looks for its data folder next to the executable (macOS: three levels up,
# as from inside an app bundle). Give it a folder in the build tree, so the tests never
# create or read the user's Dexed folder.
set(plugin_host_dir ${CMAKE_CURRENT_BINARY_DIR}/host/Contents/MacOS)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Dexed ${plugin_host_dir}/Dexed)
set_target_properties(dexed-render-plugin PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${plugin_host_dir})
foreach(config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${config} config)
    set_target_properties(dexed-render-plugin PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} ${plugin_host_dir})
endforeach()

add_test(NAME dx7-baseline-plugin COMMAND dexed-render-plugin --check ${DEXED_BASELINE})
add_test(NAME dx7-baseline-plugin-reverse COMMAND dexed-render-plugin --check ${DEXED_BASELINE} --reverse)

# The headless engine library alone, against the same baseline.
add_executable(dexed-render-engine render_engine.cpp)
target_link_libraries(dexed-render-engine PRIVATE dexed_engine)
# no JUCE here: engine headers then leave out the plugin's logger
target_compile_definitions(dexed-render-engine PRIVATE DEXED_HEADLESS=1)

add_test(NAME dx7-baseline-engine COMMAND dexed-render-engine --check ${DEXED_BASELINE})
add_test(NAME dx7-baseline-engine-reverse COMMAND dexed-render-engine --check ${DEXED_BASELINE} --reverse)
