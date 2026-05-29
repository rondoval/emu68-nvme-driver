cmake_minimum_required(VERSION 3.14.0)
project(nvme.device VERSION ${CMAKE_PROJECT_VERSION})

# Toolchain and compile options
add_link_options(
    -ffreestanding
    -nostdlib
    -nostartfiles
    -s
    -Wl,-e,_doNotExecute
)

add_compile_options(
    -Os
    -m68040
    -Wall
    -Wextra
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wmissing-prototypes
    -Wstrict-prototypes
    -DDEBUG
    -fomit-frame-pointer
    -DDEBUG_HIGH
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

target_link_libraries(nvme.device
    PRIVATE
        Emu68Common::common
        Emu68PCIe::pcie_headers
)

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
