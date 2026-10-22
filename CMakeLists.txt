cmake_minimum_required(VERSION 3.22)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(YouHost
    VERSION 1.0.10
    DESCRIPTION "Live plugin host and multitrack recorder"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.3.tar.gz
    URL_HASH SHA256=a81e5508b8a0efa483917794ebeaff56aed3075730c405a947734413f40c1aba
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_juce_bit_depth.py && python3 ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_juce_vst3_mac_scale.py && python3 ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_juce_editor_view.py && python3 ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_juce_realtime_callback.py && python3 ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_juce_au_release.py && python3 ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_juce_vst3_process.py)
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
    src/engine/SessionDisk.cpp
    src/engine/SessionDocument.cpp
    src/ui/ChannelMenu.cpp
    src/ui/CpuWindow.cpp
    src/ui/DropoutWindow.cpp
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
    YOUHOST_VERSION="${PROJECT_VERSION}"
    YOUHOST_VERSION_LABEL="YouHost ${PROJECT_VERSION}"
    YOUHOST_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources")

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
# Release adds -DNDEBUG, which compiles out the lock-reentry checks. The
# checklist builds this file without NDEBUG. -UNDEBUG has to follow -DNDEBUG.
target_compile_options(YouHostTests PRIVATE -UNDEBUG)

enable_testing()
add_test(NAME YouHostTests COMMAND YouHostTests)

# Paints SYNC LOST and fails if the Finnish line has no Ä/ä/ö glyph.
add_executable(YouHostSyncLostGlyphs tests/sync_lost_glyphs.cpp)
target_include_directories(YouHostSyncLostGlyphs PRIVATE src)
target_compile_features(YouHostSyncLostGlyphs PRIVATE cxx_std_20)
target_compile_definitions(YouHostSyncLostGlyphs PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(YouHostSyncLostGlyphs PRIVATE
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
add_test(NAME YouHostSyncLostGlyphs COMMAND YouHostSyncLostGlyphs)

# Focuses MARKERS, CPU, DROPOUTS, and LATENCY and sends keys through the
# real ComponentPeer path. A marker name editor must keep Space.
add_executable(YouHostToolKeys tests/tool_window_keys.cpp)
target_include_directories(YouHostToolKeys PRIVATE src)
target_compile_features(YouHostToolKeys PRIVATE cxx_std_20)
target_compile_definitions(YouHostToolKeys PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(YouHostToolKeys PRIVATE
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
add_test(NAME YouHostToolKeys COMMAND YouHostToolKeys)

# Loads Apple AUDelay on macOS, feeds silence into both inputs, and fails if the
# output is non-finite or nowhere near silence. The GitHub macOS job runs it.
if(APPLE)
    add_executable(YouHostAuDelayTest tests/audelay_silent.cpp)
    target_compile_features(YouHostAuDelayTest PRIVATE cxx_std_20)
    target_compile_definitions(YouHostAuDelayTest PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_AU=1
        JUCE_PLUGINHOST_VST3=0)
    target_link_libraries(YouHostAuDelayTest PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
        "-framework AudioUnit"
        "-framework CoreAudioKit"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework Cocoa")
    add_test(NAME YouHostAuDelayTest COMMAND YouHostAuDelayTest)

    # Stereo Apple AUs on a mono channel, offline, factory defaults left alone.
    # Silence must stay silent. An impulse must decay below -60 dBFS in 10 s.
    # Fails if the tail grows or the message thread does not return.
    add_executable(YouHostAuChainTest tests/au_chain.cpp)
    target_include_directories(YouHostAuChainTest PRIVATE src)
    target_compile_features(YouHostAuChainTest PRIVATE cxx_std_20)
    target_compile_definitions(YouHostAuChainTest PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_AU=1
        JUCE_PLUGINHOST_VST3=0)
    target_link_libraries(YouHostAuChainTest PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
        "-framework AudioUnit"
        "-framework CoreAudioKit"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework Cocoa")
    add_test(NAME YouHostAuChainTest COMMAND YouHostAuChainTest)

    # Real AudioEngine callback, realtime thread, 48 kHz / 32 samples, 16 Apple
    # AUs. The message thread creates, edits, recolours, and removes groups for
    # more than 30 s. Deadline misses are the callback thread's own CPU time.
    add_executable(YouHostGroupStressTest
        tests/group_stress.cpp
        src/engine/AudioEngine.cpp
        src/engine/PluginCatalogue.cpp
        src/engine/PluginRack.cpp
        src/engine/Recorder.cpp
        src/engine/SessionDisk.cpp
        src/engine/SessionDocument.cpp)
    target_include_directories(YouHostGroupStressTest PRIVATE src)
    target_compile_features(YouHostGroupStressTest PRIVATE cxx_std_20)
    target_compile_definitions(YouHostGroupStressTest PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_AU=1
        JUCE_PLUGINHOST_VST3=0)
    target_link_libraries(YouHostGroupStressTest PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
        "-framework AudioUnit"
        "-framework CoreAudioKit"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework Cocoa")
    add_test(NAME YouHostGroupStressTest COMMAND YouHostGroupStressTest)

    # 1/1 versus Fold L 2/2 on Apple AUs. The product flag has to match the level.
    add_executable(YouHostMonoNullTest tests/mono_null.cpp)
    target_include_directories(YouHostMonoNullTest PRIVATE src)
    target_compile_features(YouHostMonoNullTest PRIVATE cxx_std_20)
    target_compile_definitions(YouHostMonoNullTest PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_AU=1
        JUCE_PLUGINHOST_VST3=0)
    target_link_libraries(YouHostMonoNullTest PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
        "-framework AudioUnit"
        "-framework CoreAudioKit"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework Cocoa")
    add_test(NAME YouHostMonoNullTest COMMAND YouHostMonoNullTest)

    # Real VST3 versus AU when the runner has the same plugin in both formats.
    # Otherwise four heavy processors at 32 samples. Hidden channels are not called.
    add_executable(YouHostVst3Stress tests/vst3_au_stress.cpp)
    target_include_directories(YouHostVst3Stress PRIVATE src)
    target_compile_features(YouHostVst3Stress PRIVATE cxx_std_20)
    target_compile_definitions(YouHostVst3Stress PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_AU=1
        JUCE_PLUGINHOST_VST3=1)
    target_link_libraries(YouHostVst3Stress PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
        "-framework AudioUnit"
        "-framework CoreAudioKit"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework Cocoa")
    add_test(NAME YouHostVst3Stress COMMAND YouHostVst3Stress)
endif()
