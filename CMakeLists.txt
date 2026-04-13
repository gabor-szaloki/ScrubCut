cmake_minimum_required(VERSION 3.21)
project(ScrubCut VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Put binaries in a consistent location regardless of generator (single- or multi-config)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin)

# --- Dependencies via vcpkg ---
find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

# FFmpeg - vcpkg provides config-mode find modules
find_package(FFMPEG REQUIRED)

# --- Sources ---
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    src/*.cpp
)

# --- App icon ---
if(WIN32)
    set(APP_ICON_RC "${CMAKE_SOURCE_DIR}/platform/windows/ScrubCut.rc")
endif()

add_executable(ScrubCut ${SOURCES} ${APP_ICON_RC})


target_include_directories(ScrubCut PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(ScrubCut PRIVATE
    SDL3::SDL3
    imgui::imgui
    OpenGL::GL
    ${FFMPEG_LIBRARIES}
)

# Link ws2_32 and friends on Windows (needed by FFmpeg)
if(WIN32)
    target_link_libraries(ScrubCut PRIVATE ws2_32 bcrypt mfplat mfuuid strmiids)
endif()

# --- Install ---
if(APPLE)
    # Package as .app bundle at install time
    install(TARGETS ScrubCut RUNTIME DESTINATION "ScrubCut.app/Contents/MacOS")
    install(FILES "${CMAKE_SOURCE_DIR}/platform/macos/Info.plist" DESTINATION "ScrubCut.app/Contents")
    install(FILES "${CMAKE_SOURCE_DIR}/platform/macos/ScrubCut.icns" DESTINATION "ScrubCut.app/Contents/Resources")
    install(CODE "execute_process(COMMAND codesign --force --deep --sign - \"\${CMAKE_INSTALL_PREFIX}/ScrubCut.app\")")
elseif(WIN32)
    install(TARGETS ScrubCut RUNTIME DESTINATION ".")
endif()
