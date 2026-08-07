cmake_minimum_required(VERSION 3.22)

project(scmechanics VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "scmechanics targets Windows 10 and Windows 11")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(scmechanics_core
    src/analysis/analyzer.cpp
    src/analysis/capacity.cpp
    src/analysis/statistics.cpp
    src/cli/calibration.cpp
    src/cli/commands.cpp
    src/cli/report.cpp
    src/config/config.cpp
    src/platform/clock.cpp
    src/platform/foreground.cpp
    src/platform/raw_input.cpp
    src/capture/collector.cpp
    src/storage/session.cpp
    src/util/json.cpp
)

target_include_directories(scmechanics_core PUBLIC src)
target_compile_definitions(scmechanics_core PUBLIC
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    SCMECHANICS_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(scmechanics_core PUBLIC user32)

if(MSVC)
    target_compile_options(scmechanics_core PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

add_executable(scmechanics src/main.cpp)
target_link_libraries(scmechanics PRIVATE scmechanics_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(scmechanics_tests
        tests/test_main.cpp
        tests/test_analyzer.cpp
        tests/test_capacity.cpp
        tests/test_cli.cpp
        tests/test_ring_buffer.cpp
        tests/test_storage.cpp
        tests/test_statistics.cpp
    )
    target_link_libraries(scmechanics_tests PRIVATE scmechanics_core)
    if(MSVC)
        target_compile_options(scmechanics_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_test(NAME scmechanics_tests COMMAND scmechanics_tests)
endif()
