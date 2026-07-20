cmake_minimum_required(VERSION 3.14.0)
project(nvme-tools VERSION ${CMAKE_PROJECT_VERSION})

set(NVME_TOOL_COMPILE_OPTIONS
    -Os
    -Wall
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wmissing-prototypes
    -Wstrict-prototypes
    -Wno-unused-function
    -Wno-shift-count-overflow
)

set(NVME_TOOL_LINK_OPTIONS
    -s
)

# Debug-output backend defines (EMU68_DEBUG_BACKEND, see emu68-common).
emu68_debug_definitions()

add_executable(nvmeinfo
    nvmeinfo.c
)

add_executable(nvmeadm
    nvmeadm.c
    nvmeadm_core.c
    nvmeadm_util.c
    nvmeadm_identify.c
    nvmeadm_smart.c
    nvmeadm_logs.c
    nvmeadm_selftest.c
    nvmeadm_firmware.c
    nvmeadm_maint.c
    nvmeadm_features.c
)

target_compile_options(nvmeinfo PRIVATE ${NVME_TOOL_COMPILE_OPTIONS})
target_compile_options(nvmeadm PRIVATE ${NVME_TOOL_COMPILE_OPTIONS})

target_compile_definitions(nvmeinfo PRIVATE
    VERSTAG="$VER: nvmeinfo ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}"
)

target_compile_definitions(nvmeadm PRIVATE
    VERSTAG="$VER: nvmeadm ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}"
)

target_link_options(nvmeinfo PRIVATE ${NVME_TOOL_LINK_OPTIONS})
target_link_options(nvmeadm PRIVATE ${NVME_TOOL_LINK_OPTIONS})

target_link_libraries(nvmeinfo
    PRIVATE
        Emu68PCIe::pcie_headers
        Emu68Common::common
)

target_link_libraries(nvmeadm
    PRIVATE
        Emu68Common::common
        nvme_headers          # SDK headers: <devices/nvme.h> + <nvme/nvme_defs.h>
)

target_include_directories(nvmeinfo PRIVATE
    # SFD-generated pcie headers (proto/bcmpcie.h etc.)
    ${CMAKE_BINARY_DIR}/bcmpcie.library/generated/include
    # bcmpcie.library public headers (libraries/pcie.h, libraries/pci_constants.h, libraries/openpci.h, libraries/pcitags.h)
    ${CMAKE_SOURCE_DIR}/bcmpcie.library/include
)

# Hosted CLI programs: for the serial backend, link debug.lib (KPrintF). libc
# supplies __divsi3, so no glue and no ROM check.
emu68_debug_backend_finalize(nvmeinfo)
emu68_debug_backend_finalize(nvmeadm)

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/nvmeinfo DESTINATION C)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/nvmeadm DESTINATION C)
