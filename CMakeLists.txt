cmake_minimum_required(VERSION 3.21)
project(ScrubCut VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default install prefix per platform
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if(WIN32)
        set(CMAKE_INSTALL_PREFIX "C:/Program Files/ScrubCut" CACHE PATH "" FORCE)
    elseif(APPLE)
        set(CMAKE_INSTALL_PREFIX "/Applications" CACHE PATH "" FORCE)
    endif()
endif()

# Put binaries in a consistent location regardless of generator (single- or multi-config)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin)

# --- Dependencies via vcpkg ---
find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

# FFmpeg - vcpkg provides config-mode find modules
find_package(FFMPEG REQUIRED)

# --- Sources ---
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    src/*.cpp
)

# --- App icon + Win32 version resource ---
# The configured .rc carries both the icon and VS_VERSION_INFO (FileDescription
# etc.) — Windows uses FileDescription as the "Open With" display name.
if(WIN32)
    configure_file(
        "${CMAKE_SOURCE_DIR}/platform/windows/ScrubCut.rc.in"
        "${CMAKE_BINARY_DIR}/ScrubCut.rc"
        @ONLY
    )
    set(APP_ICON_RC "${CMAKE_BINARY_DIR}/ScrubCut.rc")
    # UTF-8 active code page manifest (merged into the exe by the manifest
    # tool) — makes narrow-string path APIs treat the UTF-8 paths we pass
    # around correctly. See the comment in the manifest itself.
    list(APPEND APP_ICON_RC "${CMAKE_SOURCE_DIR}/platform/windows/utf8.manifest")
endif()

add_executable(ScrubCut WIN32 ${SOURCES} ${APP_ICON_RC})

# --- Generated version header (project version + git hash/dirty flag) ---
set(_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(_VERSION_HEADER "${_GENERATED_DIR}/scrubcut_version.h")
file(MAKE_DIRECTORY "${_GENERATED_DIR}")
add_custom_target(generate_version
    COMMAND ${CMAKE_COMMAND}
        -DPROJECT_VERSION=${PROJECT_VERSION}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DOUTPUT_FILE=${_VERSION_HEADER}
        -P ${CMAKE_SOURCE_DIR}/cmake/GenerateVersion.cmake
    BYPRODUCTS ${_VERSION_HEADER}
    VERBATIM
)
add_dependencies(ScrubCut generate_version)

# --- Shaders: HLSL compiled at build time to blobs, embedded into a header ---
# Windows/Linux: the shadercross CLI (vcpkg host tool sdl3-shadercross)
# produces SPIRV + DXBC (SM 5.x — deliberately not DXIL, see the GPU device
# creation comment in App.cpp). macOS: vcpkg can't provision shadercross (no
# DirectX Shader Compiler port there), so glslang compiles HLSL -> SPIRV and
# spirv-cross derives MSL from it — verified to produce the same MSL interface
# as shadercross.
set(_SHADER_DIR "${CMAKE_SOURCE_DIR}/src/ui/shaders")
set(_SHADER_BLOB_DIR "${_GENERATED_DIR}/shaders")
set(_SHADER_HEADER "${_GENERATED_DIR}/scrubcut_shaders.h")
file(MAKE_DIRECTORY "${_SHADER_BLOB_DIR}")

# <basename>.<stage>.hlsl sources to compile; keep in sync with the symbol
# list in cmake/EmbedShaders.cmake.
set(_SHADER_SOURCES tonemap.vert tonemap.frag composite.frag)

# Only the formats the platform's SDL_GPU backends can consume get embedded:
# D3D12/Vulkan (Windows) and Vulkan (Linux) never load MSL, and Metal (macOS)
# never loads SPIRV/DXBC — SPIRV is produced on macOS only as the spirv-cross
# input. The other formats are embedded as empty Blobs.
set(_SHADER_BLOBS "")
if(APPLE)
    set(_SHADER_EMBED_FORMATS "msl")
    find_program(GLSLANG_EXECUTABLE
        NAMES glslang glslangValidator
        PATHS
            "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_HOST_TRIPLET}/tools/glslang"
            "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/tools/glslang"
    )
    find_program(SPIRV_CROSS_EXECUTABLE
        NAMES spirv-cross
        PATHS
            "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_HOST_TRIPLET}/tools/spirv-cross"
            "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/tools/spirv-cross"
    )
    if(NOT GLSLANG_EXECUTABLE OR NOT SPIRV_CROSS_EXECUTABLE)
        message(FATAL_ERROR "glslang / spirv-cross not found — both are needed to "
            "compile the HLSL shaders on macOS (vcpkg installs them as host tools)")
    endif()
    foreach(_shader ${_SHADER_SOURCES})
        string(REGEX REPLACE ".*\\." "" _stage "${_shader}")
        set(_spv "${_SHADER_BLOB_DIR}/${_shader}.spv")
        set(_msl "${_SHADER_BLOB_DIR}/${_shader}.msl")
        list(APPEND _SHADER_BLOBS "${_spv}" "${_msl}")
        add_custom_command(OUTPUT "${_spv}"
            COMMAND "${GLSLANG_EXECUTABLE}" -D -S ${_stage} -V
                --hlsl-iomap --auto-map-bindings -e main
                "${_SHADER_DIR}/${_shader}.hlsl" -o "${_spv}"
            DEPENDS "${_SHADER_DIR}/${_shader}.hlsl"
            COMMENT "glslang ${_shader}.spv"
            VERBATIM)
        add_custom_command(OUTPUT "${_msl}"
            COMMAND "${SPIRV_CROSS_EXECUTABLE}" "${_spv}"
                --msl --msl-version 20100
                --rename-entry-point main main0 ${_stage}
                --output "${_msl}"
            DEPENDS "${_spv}"
            COMMENT "spirv-cross ${_shader}.msl"
            VERBATIM)
    endforeach()
else()
    set(_SHADER_EMBED_FORMATS "spv,dxbc")
    find_program(SHADERCROSS_EXECUTABLE
        NAMES shadercross
        PATHS
            "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_HOST_TRIPLET}/tools/sdl3-shadercross"
            "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/tools/sdl3-shadercross"
    )
    if(NOT SHADERCROSS_EXECUTABLE)
        message(FATAL_ERROR "shadercross not found — needed to compile the HLSL "
            "shaders (vcpkg installs it as a host tool on Windows/Linux)")
    endif()
    foreach(_shader ${_SHADER_SOURCES})
        string(REGEX REPLACE ".*\\." "" _stage "${_shader}")
        if(_stage STREQUAL "vert")
            set(_stagearg vertex)
        else()
            set(_stagearg fragment)
        endif()
        foreach(_fmt spv dxbc)
            set(_blob "${_SHADER_BLOB_DIR}/${_shader}.${_fmt}")
            list(APPEND _SHADER_BLOBS "${_blob}")
            add_custom_command(OUTPUT "${_blob}"
                COMMAND "${SHADERCROSS_EXECUTABLE}"
                    "${_SHADER_DIR}/${_shader}.hlsl"
                    -s HLSL -t ${_stagearg} -o "${_blob}"
                DEPENDS "${_SHADER_DIR}/${_shader}.hlsl"
                COMMENT "shadercross ${_shader}.${_fmt}"
                VERBATIM)
        endforeach()
    endforeach()
endif()

set(_SHADER_HLSL_FILES "")
foreach(_shader ${_SHADER_SOURCES})
    list(APPEND _SHADER_HLSL_FILES "${_SHADER_DIR}/${_shader}.hlsl")
endforeach()

add_custom_target(embed_shaders
    COMMAND ${CMAKE_COMMAND}
        -DSHADER_BLOB_DIR=${_SHADER_BLOB_DIR}
        -DFORMATS=${_SHADER_EMBED_FORMATS}
        -DOUTPUT_FILE=${_SHADER_HEADER}
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedShaders.cmake
    BYPRODUCTS ${_SHADER_HEADER}
    DEPENDS ${_SHADER_BLOBS}
    SOURCES ${_SHADER_HLSL_FILES}
    VERBATIM
)
# Keep the VS generator's built-in FXC rule away from the HLSL sources —
# shadercross is the only compiler for them.
set_source_files_properties(
    ${_SHADER_HLSL_FILES}
    PROPERTIES HEADER_FILE_ONLY ON VS_TOOL_OVERRIDE "None"
)
add_dependencies(ScrubCut embed_shaders)

target_include_directories(ScrubCut PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${_GENERATED_DIR}
    ${FFMPEG_INCLUDE_DIRS}
)
# SYSTEM so warnings inside vendored headers (sprintf deprecation in
# stb_image_write.h) don't pollute our build output.
target_include_directories(ScrubCut SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(ScrubCut PRIVATE
    SDL3::SDL3
    imgui::imgui
    ${FFMPEG_LIBRARIES}
)

# Link ws2_32 and friends on Windows (needed by FFmpeg), dxgi for the native
# HDR-state query in App.cpp
if(WIN32)
    target_link_libraries(ScrubCut PRIVATE ws2_32 bcrypt mfplat mfuuid strmiids dwmapi dxgi)
endif()

# FFmpeg and SDL3 both pull in -lm/libz; Xcode 15+'s linker warns about the
# harmless duplicates. CoreFoundation backs the NFC path normalization in
# util/UnicodeNormalize.h.
if(APPLE)
    target_link_options(ScrubCut PRIVATE LINKER:-no_warn_duplicate_libraries)
    target_link_libraries(ScrubCut PRIVATE "-framework CoreFoundation")
endif()

# Subtitle fonts (Roboto + pan-CJK DroidSansFallback, both Apache-2.0) are
# loaded at runtime from next to the executable via SDL_GetBasePath(). They're
# loose files rather than embedded to keep the binary and compile times small.
# Copy them next to the binary for dev builds; the install rules below stage
# them (and their licenses) for packaging.
set(_FONT_DIR "${CMAKE_SOURCE_DIR}/third_party/fonts")
set(_FONT_FILES "Roboto-Medium.ttf" "DroidSansFallback.ttf")
set(_FONT_LICENSES "LICENSE-Roboto.txt" "LICENSE-DroidSansFallback.txt")
foreach(_f ${_FONT_FILES})
    add_custom_command(TARGET ScrubCut POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_FONT_DIR}/${_f}" "$<TARGET_FILE_DIR:ScrubCut>/${_f}"
        VERBATIM)
endforeach()

# --- Install ---

# Common CPack metadata
set(CPACK_PACKAGE_NAME "ScrubCut")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Gabor Szaloki")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Fast video scrubbing, trimming, and exporting")

if(WIN32)
    install(TARGETS ScrubCut RUNTIME DESTINATION ".")

    # Bundle vcpkg DLLs (FFmpeg, SDL3), excluding the build-time-only shader
    # toolchain DLLs that sdl3-shadercross drops into bin/
    set(_VCPKG_BIN "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/bin")
    file(GLOB _VCPKG_DLLS "${_VCPKG_BIN}/*.dll")
    list(FILTER _VCPKG_DLLS EXCLUDE REGEX "(dxcompiler|dxil|SDL3_shadercross|spirv-cross)[^/]*\\.dll$")
    install(FILES ${_VCPKG_DLLS} DESTINATION ".")

    # License and icon
    install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION ".")
    install(FILES "${CMAKE_SOURCE_DIR}/platform/windows/ScrubCut.ico" DESTINATION ".")

    # Subtitle fonts (loaded at runtime from the exe directory) + their licenses.
    foreach(_f ${_FONT_FILES} ${_FONT_LICENSES})
        install(FILES "${_FONT_DIR}/${_f}" DESTINATION ".")
    endforeach()

    # --- CPack / NSIS installer ---
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_PACKAGE_FILE_NAME "ScrubCut-${PROJECT_VERSION}-win64")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "ScrubCut")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
    # Icons
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/platform/windows/ScrubCut.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/platform/windows/ScrubCut.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "ScrubCut.exe")
    set(CPACK_NSIS_DISPLAY_NAME "ScrubCut")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)

    # Start menu shortcut. CPACK_PACKAGE_EXECUTABLES isn't used because its
    # template hardcodes $INSTDIR\bin\<exe> (we install to $INSTDIR directly).
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\ScrubCut.lnk' '$INSTDIR\\\\ScrubCut.exe' '' '$INSTDIR\\\\ScrubCut.ico'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\ScrubCut.lnk'")

    # File associations: register ScrubCut as an Open With option for video files
    set(_ASSOC_EXTS .mp4 .mkv .avi .mov .wmv .flv .webm .mpg .mpeg .3gp .ts .m4v .gif)
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
elseif(APPLE)
    # Configure Info.plist with the current PROJECT_VERSION so we don't have to
    # bump the version in two places.
    configure_file(
        "${CMAKE_SOURCE_DIR}/platform/macos/Info.plist.in"
        "${CMAKE_BINARY_DIR}/Info.plist"
        @ONLY
    )

    # Package as .app bundle at install time
    install(TARGETS ScrubCut RUNTIME DESTINATION "ScrubCut.app/Contents/MacOS")
    install(FILES "${CMAKE_BINARY_DIR}/Info.plist" DESTINATION "ScrubCut.app/Contents")
    install(FILES "${CMAKE_SOURCE_DIR}/platform/macos/ScrubCut.icns" DESTINATION "ScrubCut.app/Contents/Resources")
    # Subtitle fonts + licenses. SDL_GetBasePath() resolves to Resources/ in a bundle.
    foreach(_f ${_FONT_FILES} ${_FONT_LICENSES})
        install(FILES "${_FONT_DIR}/${_f}" DESTINATION "ScrubCut.app/Contents/Resources")
    endforeach()
    install(CODE "execute_process(COMMAND codesign --force --deep --sign - \"\${CMAKE_INSTALL_PREFIX}/ScrubCut.app\")")

    # --- CPack / DragNDrop DMG ---
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_PACKAGE_FILE_NAME "ScrubCut-${PROJECT_VERSION}-mac")
    set(CPACK_DMG_VOLUME_NAME "ScrubCut ${PROJECT_VERSION}")
    # Stage the .app at the DMG root (no install prefix prepended)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/")

    include(CPack)
endif()
