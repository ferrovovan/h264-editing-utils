# Copyright (C) 2026 h264-editing-utils contributors
#
# SPDX short: GPLv3+ — see /COPYING/COPYING at the project root.

add_executable(avcc2annexb
    main.c)

target_link_libraries(avcc2annexb PRIVATE h264common)
