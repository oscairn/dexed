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

# Oscairn's scanner and offline renderer (docs/plugin-porting.md), when an Oscairn build is
# given: -DDEXED_OSCAIRN_BUILD_DIR=<oscairn build> with oscairn-plugin-scan, oscairn-render
# and the step sequencer built.
set(DEXED_OSCAIRN_BUILD_DIR "" CACHE PATH "An Oscairn build directory, for the scanner and renderer tests")
if(DEXED_OSCAIRN_BUILD_DIR)
    if(APPLE)
        set(clap $<TARGET_BUNDLE_DIR:Dexed_CLAP>)
    else()
        set(clap $<TARGET_FILE:Dexed_CLAP>)
    endif()
    set(scan ${DEXED_OSCAIRN_BUILD_DIR}/core/oscairn-plugin-scan)
    set(render ${DEXED_OSCAIRN_BUILD_DIR}/hosts/oscairn-render)
    set(sequencer ${DEXED_OSCAIRN_BUILD_DIR}/plugins/oscairn-step-sequencer.clap)

    # The plugin's data folder next to the .clap, so it does not create one in the user's home.
    add_test(NAME oscairn-data-folder COMMAND ${CMAKE_COMMAND} -E make_directory ${clap}/../Dexed)
    set_tests_properties(oscairn-data-folder PROPERTIES FIXTURES_SETUP oscairn-data)

    add_test(NAME oscairn-scan COMMAND ${scan} --worker ${clap})
    set_tests_properties(oscairn-scan PROPERTIES PASS_REGULAR_EXPRESSION "com\\.digital-suburban\\.dexed")

    foreach(run 512 37 512-again)
        string(REGEX MATCH "^[0-9]+" block ${run})
        add_test(NAME oscairn-render-${run}
            COMMAND ${render} ${CMAKE_CURRENT_BINARY_DIR}/oscairn-${run}.wav --seconds 4 --block ${block} ${sequencer} ${clap})
        set_tests_properties(oscairn-render-${run} PROPERTIES FIXTURES_SETUP oscairn-render-${run} FIXTURES_REQUIRED oscairn-data)
    endforeach()
    add_test(NAME oscairn-render-is-repeatable COMMAND ${CMAKE_COMMAND} -E compare_files
        ${CMAKE_CURRENT_BINARY_DIR}/oscairn-512.wav ${CMAKE_CURRENT_BINARY_DIR}/oscairn-512-again.wav)
    set_tests_properties(oscairn-render-is-repeatable PROPERTIES FIXTURES_REQUIRED "oscairn-render-512;oscairn-render-512-again")
    add_test(NAME oscairn-render-is-block-size-independent COMMAND ${CMAKE_COMMAND} -E compare_files
        ${CMAKE_CURRENT_BINARY_DIR}/oscairn-512.wav ${CMAKE_CURRENT_BINARY_DIR}/oscairn-37.wav)
    set_tests_properties(oscairn-render-is-block-size-independent PROPERTIES FIXTURES_REQUIRED "oscairn-render-512;oscairn-render-37")
endif()
