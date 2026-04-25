cmake_minimum_required(VERSION 3.14.0)
project(nvme.device VERSION ${CMAKE_PROJECT_VERSION})

add_link_options(-ffreestanding -nostdlib -nostartfiles -s -Wl,-e,_doNotExecute)
add_compile_options(-Os -m68040 -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wmissing-prototypes -Wstrict-prototypes -DDEBUG -fomit-frame-pointer)
add_compile_definitions(PRIVATE DEVICE_IDSTRING="${VERSTRING}" DEVICE_VERSION=${PROJECT_VERSION_MAJOR} DEVICE_REVISION=${PROJECT_VERSION_MINOR} DEVICE_NAME="${PROJECT_NAME}" DEVICE_USE_MSI=TRUE)

add_executable(nvme.device
    src/device.c
    src/device_beginio.c
    src/device_abortio.c
    src/unit.c
    src/unit_task.c
    src/irq.c
    src/unit_commands.c
    src/nvme/nvme_probe.c
    src/nvme/nvme_scsi.c
    src/nvme/nvme_io.c
    mounter/mounter.c
    mounter/mounter_string.c
    src/device_end.c
)

target_include_directories(nvme.device PRIVATE include ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nvme.device
    PRIVATE
        Emu68Common::common
        Emu68PCIe::pcie_headers
)

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/nvme.device DESTINATION DEVS)
