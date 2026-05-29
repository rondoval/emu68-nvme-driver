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
    -DDEBUG
)

set(NVME_TOOL_LINK_OPTIONS
    -s
)

add_executable(nvmeinfo
    nvmeinfo.c
)

add_executable(nvmeadm
    nvmeadm.c
    nvmeadm_core.c
    nvmeadm_commands.c
)

target_compile_options(nvmeinfo PRIVATE ${NVME_TOOL_COMPILE_OPTIONS})
target_compile_options(nvmeadm PRIVATE ${NVME_TOOL_COMPILE_OPTIONS})

target_link_options(nvmeinfo PRIVATE ${NVME_TOOL_LINK_OPTIONS})
target_link_options(nvmeadm PRIVATE ${NVME_TOOL_LINK_OPTIONS})

target_link_libraries(nvmeinfo
    Emu68PCIe::pcie_headers
    Emu68Common::common
)

target_link_libraries(nvmeadm
    Emu68Common::common
)

target_include_directories(nvmeinfo PRIVATE
    # SFD-generated pcie headers (proto/bcmpcie.h etc.)
    ${CMAKE_BINARY_DIR}/bcmpcie.library/generated/include
    # bcmpcie.library public headers (libraries/pcie.h, libraries/pci_constants.h, libraries/openpci.h, libraries/pcitags.h)
    ${CMAKE_SOURCE_DIR}/bcmpcie.library/include
)

target_include_directories(nvmeadm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nvme.device/include
)

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/nvmeinfo DESTINATION C)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/nvmeadm DESTINATION C)
