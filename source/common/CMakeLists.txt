# Copyright (C) 2026 h264-editing-utils contributors
#
# SPDX short: GPLv3+ — see /COPYING/COPYING at the project root.

add_library(h264common STATIC
    bits.c
    annexb.c
    params.c
    index.c
    sei.c
    transcode.c)

target_include_directories(h264common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(h264common PUBLIC h264bitstream)

# --smart support: x264 + libavcodec + libavutil via pkg-config. When found,
# the definition and the import target are PUBLIC so that h264-cut and the
# unit tests inherit them. Not found (or WITH_SMART=OFF) builds the stubs in
# transcode.c.
if(WITH_SMART)
    find_package(PkgConfig REQUIRED)
    # QUIET + IMPORTED_TARGET: no hard fail when the libs are absent, but the
    # PkgConfig::H264EU_SMART import target is created when they are found.
    pkg_check_modules(H264EU_SMART QUIET IMPORTED_TARGET x264 libavcodec libavutil)
    if(H264EU_SMART_FOUND)
        target_compile_definitions(h264common PUBLIC H264EU_HAVE_SMART)
        target_link_libraries(h264common PUBLIC PkgConfig::H264EU_SMART)
    else()
        message(WARNING
            "WITH_SMART=ON: x264/libavcodec/libavutil не найдены через pkg-config — "
            "сборка без поддержки --smart (transcode_range вернёт UNSUPPORTED)")
    endif()
endif()
