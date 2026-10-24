cmake_minimum_required(VERSION 3.22)
project(KirbyAmazingMirrorRecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GBARECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gbarecomp"
    CACHE PATH "Path to the GBARecomp runtime")
set(RECOMP_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/recomp-ui"
    CACHE PATH "Path to the shared runtime UI")
set(GBARECOMP_RUNTIME_UI_ROOT "${RECOMP_UI_ROOT}"
    CACHE PATH "Path to the runtime UI sources")

if(NOT EXISTS "${GBARECOMP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "GBARecomp runtime missing: ${GBARECOMP_ROOT}")
endif()
if(NOT EXISTS "${GBARECOMP_ROOT}/external/arm-recomp-core/CMakeLists.txt" OR
   NOT EXISTS "${GBARECOMP_ROOT}/external/rbengine/CMakeLists.txt" OR
   NOT EXISTS "${GBARECOMP_ROOT}/external/recomp-net/CMakeLists.txt" OR
   NOT EXISTS "${GBARECOMP_ROOT}/platform/android/third_party/SDL/CMakeLists.txt")
    message(FATAL_ERROR "A pinned GBARecomp runtime dependency is missing.")
endif()

if(WIN32 AND NOT ANDROID)
    set(SDL2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDL2/SDL2-2.32.8")
    set(SDL2_INCLUDE_DIR "${SDL2_DIR}/include" CACHE PATH "" FORCE)
    set(SDL2_LIBRARY "${SDL2_DIR}/lib/x64/SDL2.lib" CACHE FILEPATH "" FORCE)
endif()

add_subdirectory("${GBARECOMP_ROOT}" gbarecomp_build EXCLUDE_FROM_ALL)

file(GLOB KIRBY_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/recompiled_*.cpp"
)
list(APPEND KIRBY_GENERATED_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/dispatch_table.cpp"
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/symbol_map.cpp")
    list(APPEND KIRBY_GENERATED_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/symbol_map.cpp"
    )
endif()
if(NOT KIRBY_GENERATED_SOURCES)
    message(FATAL_ERROR "No recompiled game sources found in generated/.")
endif()

if(ANDROID)
    add_library(main SHARED src/main.cpp ${KIRBY_GENERATED_SOURCES})
    set(KIRBY_TARGET main)
else()
    add_executable(kirby_port src/main.cpp ${KIRBY_GENERATED_SOURCES})
    set(KIRBY_TARGET kirby_port)
endif()

set(KIRBY_RECOMPILED_SOURCES ${KIRBY_GENERATED_SOURCES})
list(FILTER KIRBY_RECOMPILED_SOURCES INCLUDE REGEX
    "/recompiled_[0-9]+\\.cpp$")
set_source_files_properties(${KIRBY_RECOMPILED_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-O2;-g0"
)

target_include_directories(${KIRBY_TARGET} PRIVATE
    "${GBARECOMP_ROOT}/src/armv4t"
    "${GBARECOMP_ROOT}/src/gba"
    "${GBARECOMP_ROOT}/src/runtime"
    "${GBARECOMP_ROOT}/src/debug"
)
if(MSVC)
    target_link_libraries(${KIRBY_TARGET} PRIVATE
        gbarecomp_runtime
        gbarecomp_debug
        gbarecomp_gba
        gbarecomp_armv4t
    )
else()
    target_link_libraries(${KIRBY_TARGET} PRIVATE
        "-Wl,--start-group"
        gbarecomp_runtime
        gbarecomp_debug
        gbarecomp_gba
        gbarecomp_armv4t
        "-Wl,--end-group"
    )
endif()
if(ANDROID)
    set(KIRBY_GAME_CONFIG "variants/kirby_amazing_mirror/game.toml")
else()
    set(KIRBY_GAME_CONFIG "game.toml")
endif()
target_compile_definitions(${KIRBY_TARGET} PRIVATE
    GBARECOMP_DEFAULT_DEBUG_PORT=19884
    GBARECOMP_WINDOW_TITLE="KirbyAmazingMirrorRecomp"
    GBARECOMP_DEFAULT_GAME_CONFIG="${KIRBY_GAME_CONFIG}"
    GBARECOMP_BUILTIN_SHA1="274b102b6d940f46861a92b4e65f89a51815c12c"
    GBARECOMP_BUILTIN_REGION="USA"
)

if(TARGET gbarecomp_runtime)
    target_compile_definitions(gbarecomp_runtime PRIVATE KIRBY_TOUCH_SPRITES)
    set(KIRBY_IMGUI_ROOT "${RECOMP_UI_ROOT}/src/third_party/imgui")
    target_sources(gbarecomp_runtime PRIVATE
        "${RECOMP_UI_ROOT}/src/common/recomp_runtime_ui.c"
        "${RECOMP_UI_ROOT}/src/common/recomp_runtime_settings.c"
        "${RECOMP_UI_ROOT}/src/common/backends/imgui/runtime_ui_imgui.cpp"
        "${KIRBY_IMGUI_ROOT}/imgui.cpp"
        "${KIRBY_IMGUI_ROOT}/imgui_draw.cpp"
        "${KIRBY_IMGUI_ROOT}/imgui_tables.cpp"
        "${KIRBY_IMGUI_ROOT}/imgui_widgets.cpp"
        "${KIRBY_IMGUI_ROOT}/backends/imgui_impl_sdl2.cpp"
    )
    target_include_directories(gbarecomp_runtime PRIVATE
        "${KIRBY_IMGUI_ROOT}"
        "${KIRBY_IMGUI_ROOT}/backends"
    )
endif()

if(NOT ANDROID)
    if(WIN32)
        target_link_libraries(${KIRBY_TARGET} PRIVATE
            "${SDL2_DIR}/lib/x64/SDL2.lib"
            "${SDL2_DIR}/lib/x64/SDL2main.lib"
        )
        add_custom_command(TARGET ${KIRBY_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SDL2_DIR}/lib/x64/SDL2.dll"
                "$<TARGET_FILE_DIR:${KIRBY_TARGET}>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets"
                "$<TARGET_FILE_DIR:${KIRBY_TARGET}>/assets"
        )
    endif()
endif()
