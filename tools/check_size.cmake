# check_size.cmake - build 期擋下超出保留區的 image
#
# 載入器與跳板都住在 flash 最前面的 16KB。超出去就會蓋到 0x10004000 的
# 專題本體 —— 那是一個燒進去才會發現、而且症狀是「開機沒反應」的錯誤,
# 很難查。與其如此,不如讓 build 直接失敗。

if(NOT EXISTS "${IMAGE}")
    message(FATAL_ERROR "check_size: 找不到 ${IMAGE}")
endif()

file(SIZE "${IMAGE}" actual)

math(EXPR pct "${actual} * 100 / ${MAX_SIZE}")
math(EXPR headroom "${MAX_SIZE} - ${actual}")

if(actual GREATER MAX_SIZE)
    math(EXPR over "${actual} - ${MAX_SIZE}")
    message(FATAL_ERROR
        "\n"
        "  ${NAME} 佔 ${actual} bytes,超出保留區 ${MAX_SIZE} bytes 共 ${over} bytes。\n"
        "\n"
        "  這個 image 必須塞進 flash 最前面的 16KB,不然會蓋到 0x10004000\n"
        "  的專題本體。可以考慮的方向:\n"
        "    - 確認 -Os 與 --gc-sections 有生效\n"
        "    - 檢查有沒有不小心把 printf/snprintf 拉進來\n"
        "    - 真的塞不下的話,只能放大 BOOT_REGION_SIZE —— 但那要\n"
        "      同步改 common/boot_map.h、app/memmap_app.ld,並重編所有專題\n"
    )
endif()

message(STATUS "${NAME}: ${actual} / ${MAX_SIZE} bytes (${pct}%, 剩 ${headroom})")
