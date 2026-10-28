# Copyright (C) 2026 h264-editing-utils contributors
#
# SPDX short: GPLv3+ — see /COPYING/COPYING at the project root.

add_executable(mp4-cut
    main.c)

target_link_libraries(mp4-cut PRIVATE h264common)
