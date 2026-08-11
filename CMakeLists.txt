cmake_minimum_required(VERSION 3.22)

project(starcraft_mechanics_profiler VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Starcraft Mechanics Profiler targets Windows 10 and Windows 11")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(starcraft_mechanics_profiler_core
    src/analysis/analyzer.cpp
    src/analysis/macro_cycle.cpp
    src/cli/automatic_session_stats.cpp
    src/cli/calibration.cpp
    src/cli/commands.cpp
    src/cli/report.cpp
    src/config/config.cpp
    src/platform/clock.cpp
    src/platform/automatic_lifecycle.cpp
    src/platform/foreground.cpp
    src/platform/minimap_viewport_detector.cpp
    src/platform/raw_input.cpp
    src/platform/screen_region_capture.cpp
    src/platform/screen_regions.cpp
    src/capture/collector.cpp
    src/storage/session.cpp
    src/util/json.cpp
)

target_include_directories(starcraft_mechanics_profiler_core PUBLIC src)
target_compile_definitions(starcraft_mechanics_profiler_core PUBLIC
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    STARCRAFT_MECHANICS_PROFILER_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(starcraft_mechanics_profiler_core PUBLIC ole32 shell32 user32 windowscodecs)

if(MSVC)
    target_compile_options(starcraft_mechanics_profiler_core PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

add_executable(starcraft_mechanics_profiler
    src/main.cpp
    src/platform/app.rc
)
set_target_properties(starcraft_mechanics_profiler PROPERTIES OUTPUT_NAME "Starcraft Mechanics Profiler")
target_include_directories(starcraft_mechanics_profiler PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(starcraft_mechanics_profiler PRIVATE starcraft_mechanics_profiler_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(starcraft_mechanics_profiler_tests
        tests/test_main.cpp
        tests/test_analyzer.cpp
        tests/test_automatic_lifecycle.cpp
        tests/test_automatic_session_stats.cpp
        tests/test_cli.cpp
        tests/test_macro_cycle.cpp
        tests/test_minimap_viewport_detector.cpp
        tests/test_ring_buffer.cpp
        tests/test_screen_regions.cpp
        tests/test_storage.cpp
    )
    target_link_libraries(starcraft_mechanics_profiler_tests PRIVATE starcraft_mechanics_profiler_core)
    if(MSVC)
        target_compile_options(starcraft_mechanics_profiler_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_test(NAME starcraft_mechanics_profiler_tests COMMAND starcraft_mechanics_profiler_tests)
endif()
