cmake_minimum_required(VERSION 3.22)

project(starcraft_mechanics_profiler VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Starcraft Mechanics Profiler targets Windows 10 and Windows 11")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    third_party/imgui
    third_party/imgui/backends
)
target_compile_definitions(imgui PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_library(implot STATIC
    third_party/implot/implot.cpp
    third_party/implot/implot_items.cpp
)
target_include_directories(implot PUBLIC third_party/implot)
target_link_libraries(implot PUBLIC imgui)

if(MSVC)
    target_compile_options(imgui PRIVATE /W0 /EHsc /utf-8)
    target_compile_options(implot PRIVATE /W0 /EHsc /utf-8)
endif()

add_library(starcraft_mechanics_profiler_core
    src/app/application_paths.cpp
    src/app/application_controller.cpp
    src/app/gui_preferences.cpp
    src/app/game_analysis_visualization_model.cpp
    src/app/gui_single_instance.cpp
    src/app/results_view_model.cpp
    src/analysis/analyzer.cpp
    src/analysis/army_control_group.cpp
    src/analysis/production_visit.cpp
    src/analysis/replay_analysis.cpp
    src/cli/automatic_session_stats.cpp
    src/cli/automatic_session_files.cpp
    src/cli/calibration.cpp
    src/cli/commands.cpp
    src/cli/replay_readiness.cpp
    src/cli/report.cpp
    src/config/config.cpp
    src/platform/clock.cpp
    src/platform/automatic_lifecycle.cpp
    src/platform/foreground.cpp
    src/platform/minimap_viewport_detector.cpp
    src/platform/raw_input.cpp
    src/platform/screen_region_capture.cpp
    src/platform/screen_region_overlay.cpp
    src/platform/screen_regions.cpp
    src/platform/starcraft_display_mode.cpp
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

add_executable(starcraft_mechanics_profiler WIN32
    src/main.cpp
    src/app/analysis_view.cpp
    src/app/windows_application.cpp
    src/platform/app.rc
)
set_source_files_properties(src/platform/app.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/screp/screp.exe"
)
set_target_properties(starcraft_mechanics_profiler PROPERTIES OUTPUT_NAME "Starcraft Mechanics Profiler")
target_include_directories(starcraft_mechanics_profiler PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(starcraft_mechanics_profiler PRIVATE
    starcraft_mechanics_profiler_core
    imgui
    implot
    d3d11
    dwmapi
    dxgi
    gdi32
)
if(MSVC)
    target_compile_options(starcraft_mechanics_profiler PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(starcraft_mechanics_profiler_tests
        tests/test_main.cpp
        tests/test_analysis_insights_data.cpp
        tests/test_analyzer.cpp
        tests/test_army_control_group.cpp
        tests/test_automatic_lifecycle.cpp
        tests/test_automatic_session_stats.cpp
        tests/test_automatic_session_files.cpp
        tests/test_cli.cpp
        tests/test_collector.cpp
        tests/test_gui_model.cpp
        tests/test_visualization_model.cpp
        tests/test_production_visit.cpp
        tests/test_replay_readiness.cpp
        tests/test_minimap_viewport_detector.cpp
        tests/test_ring_buffer.cpp
        tests/test_screen_regions.cpp
        tests/test_scouting_regression.cpp
        tests/test_starcraft_display_mode.cpp
        tests/test_storage.cpp
    )
    target_link_libraries(starcraft_mechanics_profiler_tests PRIVATE starcraft_mechanics_profiler_core)
    if(MSVC)
        target_compile_options(starcraft_mechanics_profiler_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_test(NAME starcraft_mechanics_profiler_tests COMMAND starcraft_mechanics_profiler_tests)

    add_executable(starcraft_mechanics_profiler_replay_smoke
        tests/replay_integration_smoke.cpp
        src/platform/app.rc
    )
    target_include_directories(starcraft_mechanics_profiler_replay_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(starcraft_mechanics_profiler_replay_smoke PRIVATE
        starcraft_mechanics_profiler_core
    )
    if(MSVC)
        target_compile_options(starcraft_mechanics_profiler_replay_smoke PRIVATE
            /W4 /permissive- /EHsc /utf-8
        )
    endif()
endif()