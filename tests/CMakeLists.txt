# Render tests. See docs/oscairn-plan.md, section 4.11.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# One baseline per CPU family: arm64 (macOS, Linux ARM64) fuses multiply-adds where x86_64
# does not, so the float output filter differs in the last bits. Both were recorded from
# unmodified Dexed (commit 7f0204b), x86_64 under Rosetta, and match Linux x86_64 CI.
if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(DEXED_BASELINE ${CMAKE_CURRENT_SOURCE_DIR}/baseline/dx7-renders-x86_64.txt)
else()
    set(DEXED_BASELINE ${CMAKE_CURRENT_SOURCE_DIR}/baseline/dx7-renders-arm64.txt)
endif()

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
add_test(NAME dx7-baseline-engine-two-rates COMMAND dexed-render-engine --interleaved ${DEXED_BASELINE})

# Octet, the native CLAP (source/oscairn).
# oscairn-fm-test loads the built .clap; oscairn-fm-test-linked has the plugin linked in, so
# its allocation guard sees the plugin's allocations. Never both copies in one process.
add_executable(oscairn-fm-test oscairn_fm_test.cpp ../source/oscairn/Parameters.cpp)
target_include_directories(oscairn-fm-test PRIVATE ../libs/clap-juce-extensions/clap-libs/clap/include ../libs/oscairn-clap/include ../source)
target_link_libraries(oscairn-fm-test PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(oscairn-fm-test oscairn_fm)
add_executable(oscairn-fm-test-linked oscairn_fm_test.cpp)
target_compile_definitions(oscairn-fm-test-linked PRIVATE OSCAIRN_FM_LINKED=1)
target_link_libraries(oscairn-fm-test-linked PRIVATE oscairn_fm_objects)
set(fm_module --module $<TARGET_FILE:oscairn_fm>)
set(fm_params ${CMAKE_CURRENT_SOURCE_DIR}/baseline/oscairn-fm-params.txt)
add_test(NAME oscairn-fm-baseline COMMAND oscairn-fm-test baseline ${DEXED_BASELINE} ${fm_module})
add_test(NAME oscairn-fm-baseline-reverse COMMAND oscairn-fm-test baseline ${DEXED_BASELINE} --reverse ${fm_module})
add_test(NAME oscairn-fm-two-rates COMMAND oscairn-fm-test two-rates ${DEXED_BASELINE} ${fm_module})
add_test(NAME oscairn-fm-params COMMAND oscairn-fm-test params ${fm_params} ${fm_module})
add_test(NAME oscairn-fm-notes COMMAND oscairn-fm-test notes ${fm_module})
add_test(NAME oscairn-fm-state COMMAND oscairn-fm-test state ${fm_module})
add_test(NAME oscairn-fm-topology COMMAND oscairn-fm-test topology ${fm_module})
add_test(NAME oscairn-fm-contexts COMMAND oscairn-fm-test contexts ${fm_module})
add_test(NAME oscairn-fm-state-active COMMAND oscairn-fm-test state-active ${fm_module})
add_test(NAME oscairn-fm-no-alloc COMMAND oscairn-fm-test-linked no-alloc)
add_test(NAME oscairn-fm-threads COMMAND oscairn-fm-test-linked threads)

# Oscairn's scanner, offline renderer and plugin worker (docs/plugin-porting.md), when an
# Oscairn build is given: -DDEXED_OSCAIRN_BUILD_DIR=<oscairn build> with oscairn-plugin-scan,
# oscairn-render, oscairn-plugin-worker and the step sequencer built.
set(DEXED_OSCAIRN_BUILD_DIR "" CACHE PATH "An Oscairn build directory, for the scanner, renderer and worker tests")
if(DEXED_OSCAIRN_BUILD_DIR)
    set(scan ${DEXED_OSCAIRN_BUILD_DIR}/core/oscairn-plugin-scan)
    set(render ${DEXED_OSCAIRN_BUILD_DIR}/hosts/oscairn-render)
    set(worker ${DEXED_OSCAIRN_BUILD_DIR}/core/oscairn-plugin-worker)
    set(sequencer ${DEXED_OSCAIRN_BUILD_DIR}/plugins/oscairn-step-sequencer.clap)

    # name: test prefix; clap: the plugin; id: its CLAP ID (a regex)
    function(oscairn_host_tests name clap id)
        add_test(NAME ${name}-scan COMMAND ${scan} --worker ${clap})
        set_tests_properties(${name}-scan PROPERTIES PASS_REGULAR_EXPRESSION "${id}")
        foreach(run 512 37 512-again 512-worker)
            string(REGEX MATCH "^[0-9]+" block ${run})
            add_test(NAME ${name}-render-${run}
                COMMAND ${render} ${CMAKE_CURRENT_BINARY_DIR}/${name}-${run}.wav --seconds 4 --block ${block} ${sequencer} ${clap})
            set_tests_properties(${name}-render-${run} PROPERTIES FIXTURES_SETUP ${name}-render-${run} FIXTURES_REQUIRED oscairn-data)
        endforeach()
        # in Oscairn's plugin worker process (the *-workers pattern)
        set_tests_properties(${name}-render-512-worker PROPERTIES
            ENVIRONMENT "OSCAIRN_PLUGIN_WORKER=${worker};OSCAIRN_WORKER_BUDGET_US=50000")
        foreach(pair "repeatable;512-again" "block-size-independent;37" "same-in-worker;512-worker")
            list(GET pair 0 what)
            list(GET pair 1 other)
            add_test(NAME ${name}-render-is-${what} COMMAND ${CMAKE_COMMAND} -E compare_files
                ${CMAKE_CURRENT_BINARY_DIR}/${name}-512.wav ${CMAKE_CURRENT_BINARY_DIR}/${name}-${other}.wav)
            set_tests_properties(${name}-render-is-${what} PROPERTIES FIXTURES_REQUIRED "${name}-render-512;${name}-render-${other}")
        endforeach()
    endfunction()

    if(APPLE)
        set(dexed_clap $<TARGET_BUNDLE_DIR:Dexed_CLAP>)
    else()
        set(dexed_clap $<TARGET_FILE:Dexed_CLAP>)
    endif()
    # The desktop plugin's data folder next to the .clap, so it does not create one in the
    # user's home.
    add_test(NAME oscairn-data-folder COMMAND ${CMAKE_COMMAND} -E make_directory ${dexed_clap}/../Dexed)
    set_tests_properties(oscairn-data-folder PROPERTIES FIXTURES_SETUP oscairn-data)

    oscairn_host_tests(oscairn-dexed ${dexed_clap} "com\\.digital-suburban\\.dexed")
    oscairn_host_tests(oscairn-fm $<TARGET_FILE:oscairn_fm> "org\\.oscairn\\.fm")
endif()
