cmake_minimum_required(VERSION 3.21)
project(ch570q_wireless_debugger LANGUAGES C ASM)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(SDK "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ch570/EVT/EXAM/SRC")
set(RF_SDK "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ch570/EVT/EXAM/RF/LIB")
set(WD_RF_CHANNEL 16 CACHE STRING "WCH RF channel; identical on both boards (0..39)")
set(WD_RF_ADDRESS "0x8E89BED6" CACHE STRING "32-bit RF access address; identical on both boards")
if(NOT WD_RF_CHANNEL MATCHES "^[0-9]+$" OR WD_RF_CHANNEL GREATER 39)
    message(FATAL_ERROR "WD_RF_CHANNEL must be 0..39")
endif()
if(NOT EXISTS "${SDK}/Startup/startup_CH572.S")
    message(FATAL_ERROR "Missing official SDK; see README.md for pinned checkout instructions")
endif()
file(READ "${SDK}/Ld/Link.ld" LINKER_SOURCE)
set(LINKER_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/generated/ch570q.ld")
# Generate the GCC 8.2 compatible SDK wrapper without modifying vendor files.
file(READ "${SDK}/StdPeriphDriver/CH57x_sys.c" SYS_SOURCE)
string(FIND "${SYS_SOURCE}" "__MCPY(dst, src, (void *)((uint32_t)src+size));" MCPY_POSITION)
if(MCPY_POSITION EQUAL -1)
    message(FATAL_ERROR "SDK memcpy wrapper changed; review legacy GCC compatibility adapter")
endif()
string(REPLACE "__MCPY(dst, src, (void *)((uint32_t)src+size));"
    "memcpy(dst, src, size); /* GCC 8.2 compatibility */" SYS_SOURCE "${SYS_SOURCE}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file("${SDK}/StdPeriphDriver/CH57x_sys.c" "${CMAKE_CURRENT_BINARY_DIR}/generated/CH57x_sys.upstream" COPYONLY)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/CH57x_sys.c" "${SYS_SOURCE}")
configure_file("${SDK}/Ld/Link.ld" "${CMAKE_CURRENT_BINARY_DIR}/generated/Link.upstream.ld" COPYONLY)
string(REPLACE "LENGTH = 240K" "LENGTH = 232K" LINKER_SOURCE "${LINKER_SOURCE}")
file(WRITE "${LINKER_SCRIPT}" "${LINKER_SOURCE}\n__rf_config_reserved_start = ORIGIN(FLASH) + LENGTH(FLASH);\nASSERT(__rf_config_reserved_start == 0x3a000, \"RF configuration sectors must be reserved\")\nASSERT(_ebss + 2048 <= ORIGIN(RAM) + LENGTH(RAM), \"Insufficient RAM: reserve 2 KiB for stack\")\n")
set_source_files_properties(src/main.c src/link.c src/radio.c src/usb_dap.c src/dap.c src/swd.c src/usb_diag_main.c src/uart_log.c
    src/app.c src/platform.c src/rf_config.c src/retry.c src/uart_bridge.c
    PROPERTIES COMPILE_OPTIONS -Werror)
option(WD_UART_LOG "PA2 RX / PA3 TX 115200 diagnostic log" ON)
# Debug-only escape hatch. Full images normally force the PA2/PA3 log off
# because those pins carry the bridged target UART. Turning this on builds
# bring-up images that deliberately write log bytes onto that stream.
option(WD_UART_LOG_FULL "Allow the PA2/PA3 log in full images (debug builds)" OFF)

function(wd_firmware target)
    add_executable(${target} ${ARGN}
        "${SDK}/Startup/startup_CH572.S"
        "${SDK}/StdPeriphDriver/CH57x_clk.c"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/CH57x_sys.c"
        src/uart_log.c
        "${SDK}/StdPeriphDriver/CH57x_uart.c"
        "${SDK}/StdPeriphDriver/CH57x_gpio.c")
    if(target MATCHES "^ch570q_(auto|host_full|slave_full)$" AND NOT WD_UART_LOG_FULL)
        # PA2/PA3 are the target UART in full images; diagnostics must never
        # inject log bytes into that stream, including before/after CDC open.
        target_compile_definitions(${target} PRIVATE WD_UART_LOG=0)
    else()
        target_compile_definitions(${target} PRIVATE WD_UART_LOG=$<BOOL:${WD_UART_LOG}>)
    endif()
    set_target_properties(${target} PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${LINKER_SCRIPT}")
    target_include_directories(${target} PRIVATE include "${SDK}/StdPeriphDriver/inc" "${SDK}/RVMSIS" "${RF_SDK}")
    target_compile_options(${target} PRIVATE -Os -g3 -Wall -Wextra -ffunction-sections -fdata-sections -fno-common -fsigned-char)
    target_link_options(${target} PRIVATE -nostartfiles --specs=nano.specs --specs=nosys.specs
        "-T${LINKER_SCRIPT}" -Wl,--gc-sections -Wl,--print-memory-usage
        "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
    target_link_libraries(${target} PRIVATE "${SDK}/StdPeriphDriver/libISP572.a" m)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_OBJCOPY}" -O ihex "$<TARGET_FILE:${target}>" "${CMAKE_CURRENT_BINARY_DIR}/${target}.hex"
        COMMAND "${CMAKE_OBJCOPY}" -O binary --gap-fill 0xff "$<TARGET_FILE:${target}>" "${CMAKE_CURRENT_BINARY_DIR}/${target}.bin"
        COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:${target}>"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/${target}.hex" "${CMAKE_CURRENT_BINARY_DIR}/${target}.bin"
        VERBATIM)
endfunction()
# Historical minimal and USB bring-up images are opt-in developer tools.
option(WD_BUILD_DIAGNOSTICS "Build minimal and USB diagnostic images" OFF)
if(WD_BUILD_DIAGNOSTICS)
    wd_firmware(ch570q_placeholder src/placeholder.c)
    wd_firmware(ch570q_host src/main.c src/link.c src/radio.c src/rf_config.c src/usb_dap.c)
    wd_firmware(ch570q_slave src/main.c src/link.c src/radio.c src/rf_config.c src/dap.c src/swd.c)
    target_compile_definitions(ch570q_host PRIVATE WD_ROLE_HOST=1 WD_RF_CHANNEL=${WD_RF_CHANNEL} WD_RF_ADDRESS=${WD_RF_ADDRESS})
    target_compile_definitions(ch570q_slave PRIVATE WD_ROLE_HOST=0 WD_RF_CHANNEL=${WD_RF_CHANNEL} WD_RF_ADDRESS=${WD_RF_ADDRESS})
    target_link_libraries(ch570q_host PRIVATE "${RF_SDK}/libCH57xRF.a")
    target_link_libraries(ch570q_slave PRIVATE "${RF_SDK}/libCH57xRF.a")
endif()
# Full feature images share one application; fixed-role variants provide recovery
# without USB probing. AUTO uses a startup-only bounded USB enumeration probe.
foreach(role IN ITEMS auto host_full slave_full)
    wd_firmware(ch570q_${role} src/app.c src/platform.c src/link.c src/radio.c
        src/rf_config.c src/retry.c src/uart_bridge.c src/usb_dap.c src/dap.c src/swd.c)
    target_compile_definitions(ch570q_${role} PRIVATE WD_RF_CONFIG_FLASH_STORAGE=1
        WD_RF_CHANNEL=${WD_RF_CHANNEL} WD_RF_ADDRESS=${WD_RF_ADDRESS})
    target_link_libraries(ch570q_${role} PRIVATE "${RF_SDK}/libCH57xRF.a")
endforeach()
target_compile_definitions(ch570q_auto PRIVATE WD_ROLE_POLICY=0)
target_compile_definitions(ch570q_host_full PRIVATE WD_ROLE_POLICY=1)
target_compile_definitions(ch570q_slave_full PRIVATE WD_ROLE_POLICY=2)
if(WD_BUILD_DIAGNOSTICS)
    # Isolate USB enumeration from the radio and application scheduler.
    wd_firmware(ch570q_usb_poll src/usb_diag_main.c src/usb_dap.c)
    wd_firmware(ch570q_usb_irq src/usb_diag_main.c src/usb_dap.c)
    target_compile_definitions(ch570q_usb_poll PRIVATE WD_USB_POLLING=1)
    target_compile_definitions(ch570q_usb_irq PRIVATE WD_USB_POLLING=0)

    # Adapt the official HID baseline in the build tree; keep vendor unchanged.
    set(HID_REFERENCE "${SDK}/../USB/Device/HID_CompliantDev/src/Main.c")
    configure_file("${HID_REFERENCE}" "${CMAKE_CURRENT_BINARY_DIR}/generated/HID_reference.upstream.c" COPYONLY)
    file(READ "${HID_REFERENCE}" HID_SOURCE)
    string(REPLACE "#include \"CH57x_common.h\"" "#include \"CH57x_common.h\"\n#include \"board.h\"" HID_SOURCE "${HID_SOURCE}")
    string(REPLACE "HSECap_18p" "BOARD_HSE_CAPACITANCE" HID_SOURCE "${HID_SOURCE}")
    string(REPLACE "pEP1_RAM_Addr = EP1_Databuf;" "pEP1_RAM_Addr = EP1_Databuf;\n    pEP2_RAM_Addr = EP2_Databuf;\n    pEP3_RAM_Addr = EP3_Databuf;" HID_SOURCE "${HID_SOURCE}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/HID_reference.c" "${HID_SOURCE}")
    wd_firmware(ch570q_usb_reference "${CMAKE_CURRENT_BINARY_DIR}/generated/HID_reference.c"
        "${SDK}/StdPeriphDriver/CH57x_usbdev.c")
    target_compile_definitions(ch570q_usb_reference PRIVATE DEBUG=1)
endif()
