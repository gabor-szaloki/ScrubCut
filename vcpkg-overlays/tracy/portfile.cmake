# Client-library-only overlay of the builtin tracy port, pinned newer than
# vcpkg ships (whose patches only serve the tools features dropped here).
# Viewer/capture/csvexport binaries come prebuilt from the matching GitHub
# release — client and tools must be the same version. Delete this overlay
# (and VCPKG_OVERLAY_PORTS in CMakePresets.json) once the builtin port
# catches up.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO wolfpld/tracy
    REF "v${VERSION}"
    SHA512 53912d7563e595812b37bc55fd40508cfd8e5c42d48d957a73b6b7d18bf1287b3f795c10c9a986bf7b906d5b5bebe13b02216e563e794d0a82b2783e8ce5510b
    HEAD_REF master
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" TRACY_STATIC)

# The TRACY_* options propagate to consumers as PUBLIC compile definitions.
# TRACY_ENABLE compiles the instrumentation into all builds;
# TRACY_MANUAL_LIFETIME (implies delayed init) defers the profiler runtime —
# worker thread, listen socket — until tracy::StartupProfiler() (without it,
# the client self-initializes at load). TRACY_ON_DEMAND records only while a
# viewer is connected; TRACY_ONLY_LOCALHOST keeps the socket local.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DTRACY_STATIC=${TRACY_STATIC}
        -DTRACY_ENABLE=ON
        -DTRACY_ON_DEMAND=ON
        -DTRACY_MANUAL_LIFETIME=ON
        -DTRACY_ONLY_LOCALHOST=ON
        -DTRACY_NO_CRASH_HANDLER=ON
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME Tracy CONFIG_PATH "lib/cmake/Tracy")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
