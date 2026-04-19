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

add_executable(ScrubCut WIN32 ${SOURCES} ${APP_ICON_RC})


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

    # Bundle vcpkg DLLs (FFmpeg, SDL3)
    set(_VCPKG_BIN "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/bin")
    file(GLOB _VCPKG_DLLS "${_VCPKG_BIN}/*.dll")
    install(FILES ${_VCPKG_DLLS} DESTINATION ".")

    # License and icon
    install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION ".")
    install(FILES "${CMAKE_SOURCE_DIR}/platform/windows/ScrubCut.ico" DESTINATION ".")

    # --- CPack / NSIS installer ---
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_PACKAGE_NAME "ScrubCut")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_VENDOR "Gabor Szaloki")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Fast video scrubbing, trimming, and exporting")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "ScrubCut")
    set(CPACK_PACKAGE_EXECUTABLES "ScrubCut" "ScrubCut")

    # Icons
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/platform/windows/ScrubCut.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/platform/windows/ScrubCut.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "ScrubCut.exe")
    set(CPACK_NSIS_DISPLAY_NAME "ScrubCut")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)

    # Desktop shortcut
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$DESKTOP\\\\ScrubCut.lnk' '$INSTDIR\\\\ScrubCut.exe' '' '$INSTDIR\\\\ScrubCut.ico'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$DESKTOP\\\\ScrubCut.lnk'")

    # File associations: register ScrubCut as an Open With option for video files
    set(_ASSOC_EXTS .mp4 .mkv .avi .mov .wmv .flv .webm .mpg .mpeg .3gp .ts .m4v)
    set(_NSIS_REG_INSTALL
        "WriteRegStr HKCU 'Software\\\\Classes\\\\ScrubCut.Video' '' 'Video File - ScrubCut'")
    set(_NSIS_REG_INSTALL "${_NSIS_REG_INSTALL}\n    WriteRegStr HKCU 'Software\\\\Classes\\\\ScrubCut.Video\\\\shell\\\\open\\\\command' '' '\\\"$INSTDIR\\\\ScrubCut.exe\\\" \\\"%1\\\"'")
    set(_NSIS_REG_INSTALL "${_NSIS_REG_INSTALL}\n    WriteRegStr HKCU 'Software\\\\Classes\\\\ScrubCut.Video\\\\DefaultIcon' '' '$INSTDIR\\\\ScrubCut.ico'")
    set(_NSIS_REG_UNINSTALL
        "DeleteRegKey HKCU 'Software\\\\Classes\\\\ScrubCut.Video'")
    foreach(_ext ${_ASSOC_EXTS})
        set(_NSIS_REG_INSTALL "${_NSIS_REG_INSTALL}\n    WriteRegStr HKCU 'Software\\\\Classes\\\\${_ext}\\\\OpenWithProgids' 'ScrubCut.Video' ''")
        set(_NSIS_REG_UNINSTALL "${_NSIS_REG_UNINSTALL}\n    DeleteRegValue HKCU 'Software\\\\Classes\\\\${_ext}\\\\OpenWithProgids' 'ScrubCut.Video'")
    endforeach()
    # Notify shell of association changes
    set(_NSIS_REG_INSTALL "${_NSIS_REG_INSTALL}\n    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'")
    set(_NSIS_REG_UNINSTALL "${_NSIS_REG_UNINSTALL}\n    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'")

    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "${_NSIS_REG_INSTALL}")
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "${_NSIS_REG_UNINSTALL}")

    include(CPack)
endif()
