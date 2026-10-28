# Copyright (C) 2026 h264-editing-utils contributors
#
# SPDX short: GPLv3+ — see /COPYING/COPYING at the project root.

add_library(h264common STATIC
    bits.c
    annexb.c
    params.c
    index.c
    sei.c)

target_include_directories(h264common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(h264common PUBLIC h264bitstream)
