# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.20)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Match the FreeWili Display CPU build settings
set(PICO_PLATFORM rp2350)
set(PICO_BOARD pimoroni_pico_plus2_rp2350 CACHE STRING "Board type")

# Pico SDK version (matches freewilimain)
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if(EXISTS ${picoVscode})
    include(${picoVscode})
endif()

include(pico_sdk_import.cmake)

project(wio_e5_unlock C CXX ASM)

pico_sdk_init()

add_executable(wio_e5_unlock
    src/main.cpp
    src/rmpLib/rpSerialComm.cpp
    src/rmpLib/rpUART.cpp
    src/rmpLib/rpUARTTxPIO.cpp
    src/rmpLib/rpUARTRxPIO.cpp
    src/rmpLib/rpPIO.cpp
    src/rmpLib/rpDMA.cpp
    src/rmpLib/rpGPIO.cpp
    src/rmpLib/rpTimeStamp.cpp
    src/rmpLib/rpLoopTime.cpp
    src/rmpLib/rpBase64.cpp
)

# rpUART.cpp includes "../fwMenuMain.h" unconditionally but only references
# obMenu inside #ifndef WILI_TWO_DISPLAY — define WILI_TWO_DISPLAY here and
# point at the stub header at src/fwMenuMain.h.
target_compile_definitions(wio_e5_unlock PRIVATE WILI_TWO_DISPLAY)
target_include_directories(wio_e5_unlock PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/../wio-e5-bridge/src   # uart_protocol.h
)

target_link_libraries(wio_e5_unlock
    pico_stdlib
    hardware_i2c
    hardware_gpio
    hardware_pio
    hardware_dma
    hardware_uart
    hardware_clocks
    hardware_irq
)

pico_add_extra_outputs(wio_e5_unlock)
