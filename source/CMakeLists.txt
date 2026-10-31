set(BaseTargetName Dexed)

# The headless engine: voices, MIDI handling and rendering, without JUCE. The plugin, the
# tests and later the CLAP direct-process path link it.
add_library(dexed_engine STATIC
	msfa/dx7note.cc
	msfa/env.cc
	msfa/exp2.cc
	msfa/fm_core.cc
	msfa/fm_op_kernel.cc
	msfa/freqlut.cc
	msfa/lfo.cc
	msfa/pitchenv.cc
	msfa/sin.cc
	msfa/tuning_standard.cc
	msfa/porta.cpp

	engine/EngineMkI.cpp
	engine/EngineOpl.cpp
	engine/SynthEngine.cpp

	PluginFx.cpp

	# until MTS-ESP has is own cmake project
	../libs/MTS-ESP/Client/libMTSClient.cpp
)
target_include_directories(dexed_engine PUBLIC . ../libs/MTS-ESP/Client)
target_compile_definitions(dexed_engine PRIVATE DEXED_HEADLESS=1)
# MSFA's phase accumulators and fixed-point maths rely on signed integers wrapping around;
# make that defined behaviour (it is what the compilers already did) instead of UB.
if(NOT MSVC)
    target_compile_options(dexed_engine PRIVATE -fwrapv)
endif()
target_compile_features(dexed_engine PUBLIC cxx_std_17)
# Same optimisation flags as the plugin code it came from (flags only, no JUCE code).
target_link_libraries(dexed_engine PUBLIC tuning-library PRIVATE juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
set_target_properties(dexed_engine PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

set(DEXED_JUCE_FORMATS Standalone)

if(NOT DEXED_SKIP_VST3)
  list(APPEND DEXED_JUCE_FORMATS VST3)
endif()

if (APPLE)
  list(APPEND DEXED_JUCE_FORMATS AU)
endif()

message(STATUS "Building Dexed in formats: ${DEXED_JUCE_FORMATS}")

# Off for test and CI builds: copying installs the plugins into the user's plug-in folders.
option(DEXED_COPY_PLUGIN_AFTER_BUILD "Copy the plugins to the user's plug-in folders after building" ON)

juce_add_plugin("${BaseTargetName}"
        VERSION "1.1.0"
        ICON_BIG "../assets/ui/dexedIcon.png"
        ICON_SMALL "../assets/ui/dexedIcon.png"
        COMPANY_NAME "Digital Suburban"
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        PLUGIN_MANUFACTURER_CODE DGSB
        PLUGIN_CODE Dexd
        FORMATS ${DEXED_JUCE_FORMATS}
        PRODUCT_NAME "Dexed"
        DESCRIPTION "Dexed FM Synth"
        BUNDLE_ID "com.digitalsuburban.Dexed"
        COPY_PLUGIN_AFTER_BUILD ${DEXED_COPY_PLUGIN_AFTER_BUILD}
)

clap_juce_extensions_plugin(TARGET ${BaseTargetName}
        CLAP_ID "com.digital-suburban.dexed"
        CLAP_FEATURES instrument FM DX7)

juce_generate_juce_header(${PROJECT_NAME})

include_directories(../libs/MTS-ESP/Client .)

target_sources(${BaseTargetName} PRIVATE
	msfa/tuning.cc

	PluginProcessor.cpp

	PluginData.cpp
	Tuning.cpp

	core/Cartridge.cpp
	core/Program.cpp

    ui/PluginEditor.cpp
		ui/window/CartManager.cpp
	ui/DXLookNFeel.cpp
    ui/GlobalEditor.cpp
	ui/OperatorEditor.cpp
		ui/window/ParamDialog.cpp
		ui/window/TuningShow.cpp
	ui/component/AlgoDisplay.cpp
	ui/component/DXComponents.cpp
	ui/component/EnvDisplay.cpp
	ui/component/ProgramListBox.cpp
	ui/component/VUMeter.cpp

	midi/SysexComm.cpp
	midi/PluginMidi.cpp

	parameter/DexedApvts.cpp
	parameter/ParametersMapper.cpp
	parameter/State.cpp
    parameter/Preference.cpp

        debugger/ValueTreeDebugger.cpp
        debugger/MemoryLogger.h
)

if(DEXED_NO_ALSA)
    set(DEXED_ALSA FALSE)
    message(STATUS "Building Dexed without ALSA support")
else()
    set(DEXED_ALSA TRUE)
endif()

find_package(Jack QUIET)

target_compile_definitions(${BaseTargetName} PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_JACK=$<IF:$<BOOL:${Jack_FOUND}>,1,0>
    JUCE_ALSA=$<IF:$<BOOL:${DEXED_ALSA}>,1,0>
    JUCE_MODAL_LOOPS_PERMITTED=0 # needed for FileBrowser in CartManager
    JUCE_REPORT_APP_USAGE=0
    # JUCE_COREGRAPHICS_RENDER_WITH_MULTIPLE_PAINT_CALLS=1     # REVERT BACK SINCE THERE IS AN ISSUE WITH JUCE 7.0.0
)

target_link_libraries(${BaseTargetName} 
PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_plugin_client
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    surgesynthteam_tuningui
    tuning-library
    DexedResources
    dexed_engine
PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
)
