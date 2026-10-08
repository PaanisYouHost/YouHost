cmake_minimum_required(VERSION 3.22)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(YouHost
    VERSION 0.1.0
    DESCRIPTION "Live plugin host and multitrack recorder"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.3.tar.gz
    URL_HASH SHA256=a81e5508b8a0efa483917794ebeaff56aed3075730c405a947734413f40c1aba
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(YouHost
    PRODUCT_NAME "YouHost"
    COMPANY_NAME "Ambient Audio"
    BUNDLE_ID audio.ambient.youhost
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/YouHostIcon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "YouHost needs access to audio input so it can receive channels from your interface, including USB mixers such as the Behringer X32."
    HARDENED_RUNTIME_ENABLED FALSE
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(YouHost PRIVATE
    src/Main.cpp
    src/YouHostApplication.cpp
    src/engine/AudioEngine.cpp
    src/engine/PluginCatalogue.cpp
    src/engine/PluginRack.cpp
    src/engine/Recorder.cpp
    src/engine/SessionDocument.cpp
    src/ui/LatencyReadout.cpp
    src/ui/MeterGrid.cpp
    src/ui/MainComponent.cpp
    src/ui/PluginPage.cpp
    src/ui/ScannerWindow.cpp
    src/ui/TimelineView.cpp)

target_include_directories(YouHost PRIVATE src)

target_compile_definitions(YouHost PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_PLUGINHOST_VST3=1
    YOUHOST_VERSION="${PROJECT_VERSION}")

if(APPLE)
    target_compile_definitions(YouHost PRIVATE JUCE_PLUGINHOST_AU=1)
endif()

target_link_libraries(YouHost PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(APPLE)
    target_link_libraries(YouHost PRIVATE
        "-framework AudioUnit"
        "-framework CoreAudioKit"
        "-framework CoreAudio"
        "-framework CoreFoundation")
endif()

add_executable(YouHostTests tests/engine_tests.cpp)
target_include_directories(YouHostTests PRIVATE src)
target_compile_features(YouHostTests PRIVATE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(YouHostTests PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME YouHostTests COMMAND YouHostTests)
