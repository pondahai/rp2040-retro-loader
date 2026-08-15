cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)

# pico-sdk 的位置: -DPICO_SDK_PATH=... 與環境變數都優先,相對路徑只是
# 「checkout 就放在 pico-sdk 旁邊」時的最後手段。(沿用 infones 的寫法)
if(NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "../../pico-sdk")
endif()

include(pico_sdk_import.cmake)

project(retro_loader C CXX ASM)
pico_sdk_init()

# 16KB 是硬限制,不是目標值:超過就會蓋到 0x10004000 的專題本體。
# 這個數字必須跟 common/boot_map.h 的 BOOT_REGION_SIZE 一致。
set(BOOT_REGION_SIZE 16384)

# 兩個 target 共用的設定
function(configure_boot_image target)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/common
        ${CMAKE_CURRENT_LIST_DIR}/loader
    )
    # 空間比速度重要
    target_compile_options(${target} PRIVATE -Os -ffunction-sections -fdata-sections)
    target_link_options(${target} PRIVATE -Wl,--gc-sections)

    # 載入器不需要 serial console,而 stdio 會拖進整套 printf
    pico_enable_stdio_uart(${target} 0)
    pico_enable_stdio_usb(${target} 0)

    pico_add_extra_outputs(${target})

    # 超過 16KB 的話 build 就該失敗,不要等到燒進去才發現開不了機
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DIMAGE=$<TARGET_FILE_DIR:${target}>/${target}.bin
                -DMAX_SIZE=${BOOT_REGION_SIZE}
                -DNAME=${target}
                -P ${CMAKE_CURRENT_LIST_DIR}/tools/check_size.cmake
        VERBATIM
    )
endfunction()

# ---- 載入器: 選單 + SD 卡 + 燒錄 ------------------------------------
add_executable(loader
    loader/main.c
    loader/lcd.c
    loader/sdspi.c
    loader/fatlite.c
    loader/flasher.c
    common/launch.c
)
target_link_libraries(loader
    pico_stdlib
    hardware_spi
    hardware_flash
    hardware_sync
)
configure_boot_image(loader)

# ---- 跳板: 讓偏移過的 UF2 可以單獨燒錄 -------------------------------
add_executable(trampoline
    trampoline/trampoline.c
    common/launch.c
)
target_link_libraries(trampoline
    pico_stdlib
)
configure_boot_image(trampoline)
