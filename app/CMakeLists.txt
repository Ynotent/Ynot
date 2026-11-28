# Ynot app: one JUCE application for macOS, Windows, iOS (and Linux as a dev host).
# Android (M5) compiles these same sources from a Gradle/NDK project, because
# JUCE's CMake API does not target Android.

juce_add_binary_data(ynot_app_assets
    HEADER_NAME BinaryData.h
    NAMESPACE BinaryData
    SOURCES ${CMAKE_SOURCE_DIR}/design/tokens.json)

set(YNOT_APP_SOURCES
    src/JuceAudioDevice.cpp
    src/JucePluginHost.cpp
    src/Session.cpp
    src/MidiControl.cpp
    src/SamplerView.cpp
    src/AiPanel.cpp
    src/MidiClock.cpp
    src/MainComponent.cpp
    src/ArrangeView.cpp
    src/MixerView.cpp
    src/StepSequencerView.cpp
    src/TunePanel.cpp
    src/InspectorView.cpp
    src/PluginSandbox.cpp
    src/AudioImporter.cpp
    src/AudioExporter.cpp
    src/ExportPanel.cpp
    src/AudioIOPanel.cpp)

juce_add_gui_app(YnotApp
    PRODUCT_NAME "Ynot"
    COMPANY_NAME "Ynot"
    BUNDLE_ID "com.ynot.app"
    VERSION ${PROJECT_VERSION}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Ynot records audio from your microphone or audio interface."
    BACKGROUND_AUDIO_ENABLED TRUE
    IPHONE_SCREEN_ORIENTATIONS UIInterfaceOrientationPortrait UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
    IPAD_SCREEN_ORIENTATIONS UIInterfaceOrientationPortrait UIInterfaceOrientationPortraitUpsideDown UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
    DOCUMENT_EXTENSIONS ynot)

target_sources(YnotApp PRIVATE src/Main.cpp ${YNOT_APP_SOURCES})
if(WIN32)
    # Media Foundation: WMV/WMA/MP4/M4A/AAC import (audio from video containers too).
    target_sources(YnotApp PRIVATE src/MediaFoundationDecoder.cpp)
    target_link_libraries(YnotApp PRIVATE mf mfplat mfreadwrite mfuuid ole32)
endif()
if(APPLE)
    target_sources(YnotApp PRIVATE src/AppleAacEncoder.cpp)  # M4A export
endif()

set(YNOT_JUCE_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:YnotApp,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:YnotApp,JUCE_VERSION>"
    # VST3 hosting is desktop-only (see JucePluginHost.cpp); AU covers AUv2 on macOS and AUv3 on iOS.
    $<IF:$<STREQUAL:${CMAKE_SYSTEM_NAME},iOS>,JUCE_PLUGINHOST_VST3=0,JUCE_PLUGINHOST_VST3=1>
    JUCE_PLUGINHOST_AU=1
    JUCE_USE_ANDROID_OBOE=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # Audio import: MP3 everywhere; Windows Media (WMA/WMV, plus MP4/AAC) on Windows.
    # Core Audio (M4A/AAC/MP4) is on by default on Apple platforms.
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_WINDOWS_MEDIA_FORMAT=1)

if(YNOT_ASIO_SDK_DIR)
    list(APPEND YNOT_JUCE_DEFS JUCE_ASIO=1)
    set(YNOT_ASIO_INCLUDE "${YNOT_ASIO_SDK_DIR}/common")
endif()

set(YNOT_JUCE_MODULES
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_devices
    juce::juce_gui_extra)

target_compile_definitions(YnotApp PRIVATE ${YNOT_JUCE_DEFS})
target_include_directories(YnotApp PRIVATE ${YNOT_ASIO_INCLUDE})
target_link_libraries(YnotApp PRIVATE
    ynot_core ynot_app_assets ${YNOT_JUCE_MODULES}
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

if(YNOT_BUILD_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    juce_add_console_app(YnotAppTests PRODUCT_NAME "YnotAppTests")
    target_sources(YnotAppTests PRIVATE tests/AppTests.cpp src/JucePluginHost.cpp src/JuceAudioDevice.cpp src/PluginSandbox.cpp src/AudioImporter.cpp)
    if(WIN32)
        target_sources(YnotAppTests PRIVATE src/MediaFoundationDecoder.cpp)
        target_link_libraries(YnotAppTests PRIVATE mf mfplat mfreadwrite mfuuid ole32)
    endif()
    target_compile_definitions(YnotAppTests PRIVATE ${YNOT_JUCE_DEFS}
        YNOT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    # Same JUCE modules as the app: plugin hosting on macOS needs frameworks (e.g. CoreAudioKit
    # for AU generic views) that only the full module set pulls in.
    target_link_libraries(YnotAppTests PRIVATE
        ynot_core ${YNOT_JUCE_MODULES} juce::juce_recommended_config_flags)
    if(APPLE)
        target_link_libraries(YnotAppTests PRIVATE "-framework CoreAudioKit")
    endif()
    add_test(NAME ynot_app_tests COMMAND YnotAppTests)
endif()
