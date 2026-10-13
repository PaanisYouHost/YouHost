cmake_minimum_required(VERSION 3.22)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(YouMixPse
    VERSION 1.0.0
    DESCRIPTION "YouMix and PSE standalone plugins"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.3.tar.gz
    URL_HASH SHA256=a81e5508b8a0efa483917794ebeaff56aed3075730c405a947734413f40c1aba
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(JUCE)

set(PLUGIN_FORMATS VST3)
if(APPLE)
    list(APPEND PLUGIN_FORMATS AU)
endif()

add_executable(YouMixPseTests tests/dsp_tests.cpp)
target_include_directories(YouMixPseTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(YouMixPseTests PRIVATE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(YouMixPseTests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
endif()

function(add_ambient_plugin target product bundle code)
    juce_add_plugin(${target}
        COMPANY_NAME "Ambient Audio"
        BUNDLE_ID "${bundle}"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE
        PLUGIN_MANUFACTURER_CODE Ambt
        PLUGIN_CODE ${code}
        FORMATS ${PLUGIN_FORMATS}
        PRODUCT_NAME "${product}"
        AU_MAIN_TYPE kAudioUnitType_Effect
        VST3_CATEGORIES Dynamics)
    target_sources(${target} PRIVATE ${ARGN})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(${target} PRIVATE
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endfunction()

add_ambient_plugin(YouMix "YouMix" audio.ambient.youmix Ymx1
    YouMix/PluginProcessor.cpp
    YouMix/PluginEditor.cpp)

add_ambient_plugin(PSE "PSE" audio.ambient.pse Pse1
    PSE/PluginProcessor.cpp
    PSE/PluginEditor.cpp)
