# Generate a header with project version + git hash + dirty flag + whether
# HEAD is the release tag (v<PROJECT_VERSION>).
#
# Invoked at build time via: cmake -P GenerateVersion.cmake
# Inputs (passed as -DVAR=value):
#   PROJECT_VERSION  - x.y.z from CMakeLists project()
#   SOURCE_DIR       - top of the source tree (where .git lives)
#   OUTPUT_FILE      - full path of the header to write
#
# Only writes the file if its content would change, to avoid forcing a rebuild
# on every invocation when nothing has changed.

find_package(Git QUIET)

set(GIT_HASH "unknown")
set(GIT_DIRTY 0)
set(GIT_ON_RELEASE_TAG 0)

if(GIT_EXECUTABLE AND EXISTS "${SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE _hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc
        ERROR_QUIET
    )
    if(_rc EQUAL 0 AND NOT _hash STREQUAL "")
        set(GIT_HASH "${_hash}")
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} status --porcelain
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE _status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT _status STREQUAL "")
        set(GIT_DIRTY 1)
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} tag --points-at HEAD
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE _tags
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    string(REPLACE "\n" ";" _tags "${_tags}")
    if("v${PROJECT_VERSION}" IN_LIST _tags)
        set(GIT_ON_RELEASE_TAG 1)
    endif()
endif()

set(_new
"#pragma once
#define SCRUBCUT_VERSION \"${PROJECT_VERSION}\"
#define SCRUBCUT_GIT_HASH \"${GIT_HASH}\"
#define SCRUBCUT_GIT_DIRTY ${GIT_DIRTY}
#define SCRUBCUT_GIT_ON_RELEASE_TAG ${GIT_ON_RELEASE_TAG}
")

set(_old "")
if(EXISTS "${OUTPUT_FILE}")
    file(READ "${OUTPUT_FILE}" _old)
endif()

# "" = clean release tag, "*" = clean but not the release tag, "**" = dirty
if(GIT_DIRTY)
    set(_dirty "**")
elseif(NOT GIT_ON_RELEASE_TAG)
    set(_dirty "*")
else()
    set(_dirty "")
endif()

if(_new STREQUAL _old)
    message(STATUS "Version unchanged: v${PROJECT_VERSION} - ${GIT_HASH}${_dirty}")
else()
    file(WRITE "${OUTPUT_FILE}" "${_new}")
    message(STATUS "Version updated: v${PROJECT_VERSION} - ${GIT_HASH}${_dirty}")
endif()
