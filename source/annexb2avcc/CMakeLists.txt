# Copyright (C) 2026 h264-editing-utils contributors
#
# SPDX short: GPLv3+ — see /COPYING/COPYING at the project root.

add_executable(annexb2avcc
    main.c)

target_link_libraries(annexb2avcc PRIVATE h264common)
