cmake_minimum_required(VERSION 3.14.0)
project(nvme.device VERSION ${CMAKE_PROJECT_VERSION})

# Debug-output backend defines (EMU68_DEBUG_BACKEND, see emu68-common).
emu68_debug_definitions()

# cache_ops.h LVO-fallback define (EMU68_FORCE_LVO_CACHE_OPS, see emu68-common).
emu68_cache_ops_definitions()

# Toolchain and compile options
add_link_options(
    -ffreestanding
    -nostdlib
    -nostartfiles
    -s
    -Wl,-e,_doNotExecute
)

add_compile_options(
    -O3
    -m68040
    -Wall
    -Wextra
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wmissing-prototypes
    -Wstrict-prototypes
    -fomit-frame-pointer
)

add_compile_definitions(
    PRIVATE
        DEVICE_IDSTRING="${VERSTRING}"
        DEVICE_VERSION=${PROJECT_VERSION_MAJOR}
        DEVICE_REVISION=${PROJECT_VERSION_MINOR}
        DEVICE_NAME="${PROJECT_NAME}"
        DEVICE_USE_MSI=TRUE
)

# Sources
add_executable(nvme.device
    src/device.c
    src/device_beginio.c
    src/device_abortio.c
    src/unit.c
    src/unit_task.c
    src/admin_task.c
    src/irq.c
    src/unit_commands.c
    src/unit_commands_scsi.c
    src/nvme/nvme_probe.c
    src/nvme/nvme_io.c
    src/nvme/nvme_queue.c
    src/nvme/nvme_hmb.c
    src/nvme/nvme_constants.c
    src/nvme/kcompat.c
    src/nvme/nvme_admin.c
    src/nvme/nvme_passthru.c
    src/nvme/nvme_aen.c
    src/nvme/nvme_ctrl.c
    src/nvme/nvme_quirks.c
    src/nvme/nvme_fw.c
    src/nvme/nvme_identify.c
    src/nvme/nvme_completion.c
    src/nvme/nvme_scan.c
    mounter/mounter.c
    src/device_end.c
)

target_include_directories(nvme.device
    PRIVATE
        include
        mounter
)

# Route the mounter submodule's diagnostics into the debug backend; device.c
# provides the mounter_log sink.  The submodule keeps its own switch names, so
# map them onto our tiers: its printf() carries mount errors (debug tier), its
# dbg() is per-step tracing (trace tier).  Both compile away below those.
emu68_tier_at_least(_mounter_log debug)
if(_mounter_log)
    target_compile_definitions(nvme.device PRIVATE MOUNTER_LOG)
endif()
emu68_tier_at_least(_mounter_trace trace)
if(_mounter_trace)
    target_compile_definitions(nvme.device PRIVATE MOUNTER_TRACE=1)
endif()

# Third-party submodule; not -Wconversion-clean under this project's strict
# warning set.  Bugfixes go to the fork (branch poseidon-fixes).
set_source_files_properties(mounter/mounter.c PROPERTIES
    COMPILE_OPTIONS "-Wno-conversion;-Wno-sign-conversion;-Wno-missing-prototypes;-Wno-pointer-sign;-Wno-shadow;-Wno-unused-parameter;-Wno-empty-body")

target_link_libraries(nvme.device
    PRIVATE
        Emu68Common::common
        Emu68PCIe::pcie_headers
        gcc                # libgcc runtime helpers for remaining 64-bit ops on m68k,
                           # notably nvme_scan.c's u64 LBA->sector shift (__ashldi3)
                           # and nvme_hmb.c's 64-bit ceil-divide for hmmaxd_cap (__udivdi3)
)

# ROM-able: no writable .data/.bss may sneak in. For the serial backend this
# instead links debug.lib (+ __divsi3 glue) and skips the ROM check.
emu68_debug_backend_finalize(nvme.device ROMABLE)

install(
    PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/nvme.device
    DESTINATION DEVS
)

# ------------------------------------------------------------------ #
# SDK: public passthrough interface (devices/nvme.h) + NVMe spec      #
# definitions (nvme/nvme_defs.h), shipped as an NVMe:: headers target #
# with a find_package() config.  Mirrors emu68-gic400-library.        #
# ------------------------------------------------------------------ #
add_library(nvme_headers INTERFACE)
target_include_directories(nvme_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(nvme_headers INTERFACE Emu68Common::common)

# Only the public surface is installed — internal headers (nvme_core.h,
# device.h, kcompat.h, the other nvme/*.h) stay private.
install(FILES include/devices/nvme.h   DESTINATION include/devices)
install(FILES include/nvme/nvme_defs.h DESTINATION include/nvme)

install(
    TARGETS nvme_headers
    EXPORT NVMeTargets
)
install(
    EXPORT NVMeTargets
    NAMESPACE NVMe::
    FILE NVMeTargets.cmake
    DESTINATION lib/cmake/NVMe
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/NVMeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/NVMeConfig.cmake
    INSTALL_DESTINATION lib/cmake/NVMe
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/NVMeConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/NVMeConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/NVMeConfigVersion.cmake
    DESTINATION lib/cmake/NVMe
)
