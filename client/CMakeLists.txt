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
)

target_link_libraries(Grandiarchipelago PRIVATE
    d3d11
    dxgi
    gdi32
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
