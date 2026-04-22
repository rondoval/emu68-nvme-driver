cmake_minimum_required(VERSION 3.14.0)
project(nvmeinfo VERSION ${CMAKE_PROJECT_VERSION})

add_compile_options(-Os -Wall -Wconversion -Wsign-conversion -Wshadow -Wmissing-prototypes -Wstrict-prototypes -Wno-unused-function -Wno-shift-count-overflow -DDEBUG)
add_link_options(-s)

add_executable(nvmeinfo
    nvmeinfo.c
)

target_link_libraries(nvmeinfo
    Emu68PCIe::pcie_headers
     Emu68Common::common
)

target_include_directories(nvmeinfo PRIVATE
    # SFD-generated pcie headers (proto/bcmpcie.h etc.)
    ${CMAKE_BINARY_DIR}/bcmpcie.library/generated/include
    # bcmpcie.library public headers (libraries/pcie.h, libraries/pci_constants.h, libraries/openpci.h, libraries/pcitags.h)
    ${CMAKE_SOURCE_DIR}/bcmpcie.library/include
)

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/nvmeinfo DESTINATION C)
