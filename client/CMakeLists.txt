cmake_minimum_required(VERSION 3.20)
project(GrandiarchipelagoClient LANGUAGES CXX)

# Grandia HD Remaster is a 32-bit native port (grandia.exe). Build Win32 only.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(WARNING "Configure with -A Win32 (32-bit). Grandia is not a 64-bit process.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(Grandiarchipelago SHARED
    Grandiarchipelago/dllmain.cpp
    Grandiarchipelago/hooks.cpp
    Grandiarchipelago/item_tracker.cpp
    Grandiarchipelago/ap_session.cpp
    Grandiarchipelago/pipe_bridge.cpp
    Grandiarchipelago/log.cpp
    Grandiarchipelago/game_memory.cpp
    Grandiarchipelago/chest_pickup.cpp
    Grandiarchipelago/stash_watcher.cpp
    Grandiarchipelago/save_sync.cpp
    Grandiarchipelago/map_travel.cpp
    Grandiarchipelago/d3d_overlay.cpp
    Grandiarchipelago/debug_mode.cpp
    Grandiarchipelago/map_overview.cpp
    Grandiarchipelago/xp_multiplier.cpp
    Grandiarchipelago/speed_turbo.cpp
    Grandiarchipelago/movie_skip.cpp
    Grandiarchipelago/m_dat_balance.cpp
    Grandiarchipelago/windt_balance.cpp
    Grandiarchipelago/shop_balance.cpp
)

target_link_libraries(Grandiarchipelago PRIVATE
    d3d11
    dxgi
    gdi32
    Xinput9_1_0
)

target_include_directories(Grandiarchipelago PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Grandiarchipelago
)

target_compile_definitions(Grandiarchipelago PRIVATE GRANDIA_AP_EXPORTS)

add_executable(GrandiaLauncher
    launcher/main.cpp
)

if(MSVC)
    target_compile_options(Grandiarchipelago PRIVATE /W4 /utf-8)
    # Static CRT so the DLL loads without VC++ redist on the game PC.
    set_property(TARGET Grandiarchipelago PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(GrandiaLauncher PRIVATE /W4 /utf-8)
    set_property(TARGET GrandiaLauncher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

install(TARGETS Grandiarchipelago GrandiaLauncher
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin
)

# Keep worlds/grandia/native in sync for Archipelago Launcher / apworld packaging.
# Redux shop stock lives in map MDPs (e.g. 204C.MDP) + SHOP.BIN — stage via Python.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_EXECUTABLE)
    set(Python3_EXECUTABLE "python")
endif()
set(_STAGE_REDUX "${CMAKE_SOURCE_DIR}/../tools/stage_redux_field_overlays.py")
add_custom_command(TARGET Grandiarchipelago POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_SOURCE_DIR}/../worlds/grandia/native"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Grandiarchipelago>"
            "${CMAKE_SOURCE_DIR}/../worlds/grandia/native/Grandiarchipelago.dll"
    COMMAND "${Python3_EXECUTABLE}" "${_STAGE_REDUX}"
            --dest "${CMAKE_SOURCE_DIR}/../worlds/grandia/native/redux_content"
    COMMAND "${Python3_EXECUTABLE}" "${_STAGE_REDUX}"
            --dest "$<TARGET_FILE_DIR:Grandiarchipelago>/redux_content"
    COMMENT "Copy Grandiarchipelago.dll + stage Redux FIELD overlays (SHOP.BIN + MDPs)"
)
