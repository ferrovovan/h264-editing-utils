# Copyright (C) 2026 h264-editing-utils contributors
#
# SPDX short: GPLv3+ — see /COPYING/COPYING at the project root.

add_executable(h264-cut
    main.c)

target_link_libraries(h264-cut PRIVATE h264common)
