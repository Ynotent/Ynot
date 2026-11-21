cmake_minimum_required(VERSION 3.22)
project(Ynot VERSION 0.17.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(YNOT_BUILD_TESTS "Build shared-core unit tests" ON)
option(YNOT_BUILD_CLI "Build the ynot command-line tool" ON)
option(YNOT_BUILD_APP "Build the JUCE application (fetches JUCE unless YNOT_JUCE_DIR is set)" OFF)
set(YNOT_JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional)")
set(YNOT_ASIO_SDK_DIR "" CACHE PATH "Path to Steinberg's ASIO SDK (Windows, optional)")

# ---------------------------------------------------------------- shared core
# Pure C++20, no OS audio/UI dependencies. Linked by every platform shell:
#   macOS / iOS  -> static library inside the Xcode targets
#   Windows      -> static library inside the desktop app
#   Android      -> built by the NDK, exposed to Kotlin via JNI
add_library(ynot_core STATIC
    core/src/json.cpp
    core/src/platform.cpp
    core/src/model.cpp
    core/src/media.cpp
    core/src/project_format.cpp
    core/src/engine.cpp
    core/src/compatibility.cpp
    core/src/collab.cpp
    core/src/audio_device.cpp
    core/src/ui_layout.cpp
    core/src/account.cpp
    core/src/demo.cpp
    core/src/recorder.cpp
    core/src/edit.cpp
    core/src/midi_recorder.cpp
    core/src/quantize.cpp
)
target_include_directories(ynot_core PUBLIC core/include)
if(MSVC)
    target_compile_options(ynot_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(ynot_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Mobile cross-compiles only need the library.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR ANDROID)
    set(YNOT_BUILD_TESTS OFF)
    set(YNOT_BUILD_CLI OFF)
endif()

if(YNOT_BUILD_APP)
    if(YNOT_JUCE_DIR)
        add_subdirectory(${YNOT_JUCE_DIR} ${CMAKE_BINARY_DIR}/juce EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(juce
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 8.0.4
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(juce)
    endif()
endif()

if(YNOT_BUILD_CLI)
    add_executable(ynot tools/ynot-cli/main.cpp)
    target_link_libraries(ynot PRIVATE ynot_core)
endif()

if(YNOT_BUILD_TESTS)
    enable_testing()
    add_executable(ynot_core_tests
        core/tests/test_main.cpp
        core/tests/test_json.cpp
        core/tests/test_project_format.cpp
        core/tests/test_engine.cpp
        core/tests/test_compatibility.cpp
        core/tests/test_collab.cpp
        core/tests/test_platform.cpp
        core/tests/test_recorder.cpp
        core/tests/test_edit.cpp
        core/tests/test_midi.cpp
        core/tests/test_quantize.cpp
    )
    target_link_libraries(ynot_core_tests PRIVATE ynot_core)
    add_test(NAME ynot_core_tests COMMAND ynot_core_tests)
endif()

if(YNOT_BUILD_APP)
    add_subdirectory(app)
endif()
