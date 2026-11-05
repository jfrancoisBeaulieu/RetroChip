cmake_minimum_required(VERSION 3.22)
project(RetroChip VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The sources are UTF-8, French text included. Visual C++ reads them in the system's code page unless told.
if(MSVC)
    add_compile_options(/utf-8)
    set(RETROCHIP_WARNINGS /W4)
else()
    set(RETROCHIP_WARNINGS -Wall -Wextra)
endif()

foreach(dependency JUCE ymfm TinySoundFont)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/${dependency}")
        message(FATAL_ERROR "external/${dependency} is missing: see the build section of README.md")
    endif()
endforeach()

enable_testing()

add_library(retrochip_engine STATIC
    engine/BandLimitedBuffer.cpp
    engine/NesVoice.cpp
    engine/NesEngine.cpp
    engine/Resampler.cpp
    engine/genesis/GenesisEngine.cpp
    engine/genesis/PsgEngine.cpp
    engine/genesis/Tfi.cpp
    engine/nes/Dpcm.cpp
    engine/nes/Fti.cpp
    engine/nes/N163Waves.cpp
    engine/nes/NesSequence.cpp
    engine/snes/Brr.cpp
    engine/snes/BrrEncoder.cpp
    engine/snes/SnesBank.cpp
    engine/snes/SnesEcho.cpp
    engine/snes/SnesEngine.cpp
    engine/soundfont/SoundFontBank.cpp
    engine/soundfont/SoundFontEngine.cpp)
target_include_directories(retrochip_engine PUBLIC engine)
target_compile_options(retrochip_engine PRIVATE ${RETROCHIP_WARNINGS})

# Only the files ymfm_opn.cpp needs. Third-party code, so its warnings stay off.
set(RETROCHIP_YMFM_SOURCES
    external/ymfm/src/ymfm_opn.cpp
    external/ymfm/src/ymfm_adpcm.cpp
    external/ymfm/src/ymfm_ssg.cpp)
target_sources(retrochip_engine PRIVATE ${RETROCHIP_YMFM_SOURCES})
target_include_directories(retrochip_engine SYSTEM PRIVATE external/ymfm/src)
if(MSVC)
    set_source_files_properties(${RETROCHIP_YMFM_SOURCES} PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(${RETROCHIP_YMFM_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

# TinySoundFont is one header, which SoundFontBank.cpp turns into code. A system include path keeps
# its warnings off while the rest of that file is still checked.
target_include_directories(retrochip_engine SYSTEM PRIVATE external/TinySoundFont)

add_executable(engine_tests tests/engine_tests.cpp)
target_link_libraries(engine_tests PRIVATE retrochip_engine)
add_test(NAME engine_tests COMMAND engine_tests)

add_subdirectory(external/JUCE)

set(RETROCHIP_PLUGIN_SOURCES
    plugin/PluginProcessor.cpp
    plugin/Localisation.cpp
    plugin/PluginEditor.cpp
    plugin/SampleImporter.cpp
    plugin/SoundFontLibrary.cpp
    plugin/SoundFontLoader.cpp)

set(RETROCHIP_JUCE_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

set(RETROCHIP_JUCE_MODULES
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_basics
    juce::juce_graphics
    juce::juce_core)

# On macOS the build installs the plugin in ~/Library/Audio/Plug-Ins, where Logic finds it. On Windows the VST3 folder
# needs an administrator, so the build leaves the plugin in build/RetroChip_artefacts; see the README.
if(APPLE)
    set(RETROCHIP_COPY_AFTER_BUILD TRUE)
else()
    set(RETROCHIP_COPY_AFTER_BUILD FALSE)
endif()

juce_add_plugin(RetroChip
    COMPANY_NAME "SleeperMusic"
    COMPANY_COPYRIGHT "Copyright (C) 2026 SleeperMusic. Licensed under the GNU AGPLv3."
    BUNDLE_ID com.sleepermusic.retrochip
    PRODUCT_NAME "RetroChip"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    PLUGIN_MANUFACTURER_CODE Slpr
    PLUGIN_CODE Rc2a
    FORMATS AU VST3 Standalone
    VST3_CATEGORIES Instrument Synth
    AU_SANDBOX_SAFE TRUE
    COPY_PLUGIN_AFTER_BUILD ${RETROCHIP_COPY_AFTER_BUILD})

target_sources(RetroChip PRIVATE ${RETROCHIP_PLUGIN_SOURCES})
target_compile_definitions(RetroChip PUBLIC ${RETROCHIP_JUCE_DEFINITIONS})
target_link_libraries(RetroChip
    PRIVATE
        retrochip_engine
        ${RETROCHIP_JUCE_MODULES}
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

juce_add_console_app(plugin_tests PRODUCT_NAME "RetroChipPluginTests")
target_sources(plugin_tests PRIVATE tests/plugin_tests.cpp ${RETROCHIP_PLUGIN_SOURCES})
target_include_directories(plugin_tests PRIVATE plugin)
target_compile_definitions(plugin_tests PRIVATE ${RETROCHIP_JUCE_DEFINITIONS}
    JUCE_MODAL_LOOPS_PERMITTED=1
    RETROCHIP_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/reference"
    RETROCHIP_SNAPSHOT_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(plugin_tests PRIVATE retrochip_engine ${RETROCHIP_JUCE_MODULES}
    juce::juce_recommended_config_flags)
add_test(NAME plugin_tests COMMAND plugin_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Loads the built bundles as a DAW does, through their VST3 and Audio Unit interfaces.
juce_add_console_app(host_tests PRODUCT_NAME "RetroChipHostTests")
target_sources(host_tests PRIVATE tests/host_tests.cpp)
target_compile_definitions(host_tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    $<$<BOOL:${APPLE}>:JUCE_PLUGINHOST_AU=1>
    JUCE_MODAL_LOOPS_PERMITTED=1
    RETROCHIP_VST3_PATH="$<GENEX_EVAL:$<TARGET_PROPERTY:RetroChip_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
target_link_libraries(host_tests PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_recommended_config_flags)
add_dependencies(host_tests RetroChip_VST3)
if(APPLE)
    add_dependencies(host_tests RetroChip_AU)
endif()
add_test(NAME host_tests COMMAND host_tests)
