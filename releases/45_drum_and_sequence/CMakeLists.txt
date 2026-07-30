# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(drum_and_sequence C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

set(COMPUTERCARD_DIR ${CMAKE_CURRENT_LIST_DIR}/../../Demonstrations+HelloWorlds/PicoSDK/ComputerCard)

add_executable(drum_and_sequence
    main.cpp
    shared_state.cpp
    sample_bank.cpp
    voice_cache.cpp
    sample_manager.cpp
    pattern_store.cpp
    usb_core1.cpp
    usb_descriptors.c
    midi_sysex.cpp
)

target_compile_options(drum_and_sequence PRIVATE -Wdouble-promotion -Wfloat-conversion -Wall -Wextra)
target_link_options(drum_and_sequence PRIVATE -Wl,--print-memory-usage)
target_compile_definitions(drum_and_sequence PRIVATE
    PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
    PICO_FLASH_SIZE_BYTES=16777216
    DS_FIRMWARE_RESERVE=262144u
)

target_include_directories(drum_and_sequence PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${COMPUTERCARD_DIR}
)

target_link_libraries(drum_and_sequence
    pico_unique_id pico_stdlib pico_multicore
    hardware_dma hardware_i2c hardware_pwm hardware_adc hardware_spi hardware_clocks hardware_flash
    tinyusb_device tinyusb_board
)

pico_set_binary_type(drum_and_sequence copy_to_ram)
pico_add_extra_outputs(drum_and_sequence)
pico_enable_stdio_usb(drum_and_sequence 0)

add_custom_command(TARGET drum_and_sequence POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE_DIR:drum_and_sequence>/drum_and_sequence.uf2"
        "${CMAKE_CURRENT_LIST_DIR}/drum_and_sequence.uf2"
    COMMENT "Copying drum_and_sequence.uf2 to release folder")

add_custom_command(
    TARGET drum_and_sequence
    POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DBIN_FILE="$<TARGET_FILE_DIR:drum_and_sequence>/drum_and_sequence.bin"
        -DFIRMWARE_RESERVE=262144
        -P "${CMAKE_CURRENT_LIST_DIR}/cmake/check_firmware_reserve.cmake"
)
