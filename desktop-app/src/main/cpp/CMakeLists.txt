cmake_minimum_required(VERSION 3.15)
project(player_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(JNI REQUIRED)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${JNI_INCLUDE_DIRS}
)

if(WIN32)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/webview2/build/native/include)
    
    set(SOURCES
        common/mpv_core.cpp
        windows/surface_win32.cpp
        windows/webview_win32.cpp
    )
    
    add_library(player_bridge SHARED ${SOURCES})
    target_link_libraries(player_bridge PRIVATE
        ole32 user32 dwmapi advapi32 gdi32 uuid
    )
    set_target_properties(player_bridge PROPERTIES
        PREFIX ""
        SUFFIX ".dll"
    )
elseif(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    find_package(X11 REQUIRED)
    find_package(OpenGL REQUIRED)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
    pkg_check_modules(WEBKITGTK REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
    # The bridge uses MPV's client/render headers but resolves its symbols at
    # runtime. Keep libmpv out of DT_NEEDED so Java/JNA and the bridge share
    # one selected system ABI instead of loading two copies.
    pkg_check_modules(MPV REQUIRED mpv)

    set(SOURCES
        common/mpv_core.cpp
        linux/surface_linux.cpp
    )
    
    add_library(player_bridge SHARED ${SOURCES})
    target_include_directories(player_bridge PRIVATE ${MPV_INCLUDE_DIRS})
    target_link_libraries(player_bridge PRIVATE
        pthread dl
        X11::X11
        ${OPENGL_LIBRARIES}
        PkgConfig::GTK3
        PkgConfig::WEBKITGTK
    )
    set_target_properties(player_bridge PROPERTIES
        PREFIX "lib"
        SUFFIX ".so"
    )
endif()
