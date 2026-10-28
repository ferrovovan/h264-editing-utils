# Copyright (C) 2026 h264-editing-utils contributors
#
# SPDX short: GPLv3+ — see /COPYING/COPYING at the project root.
#
# h264bitstream is imported as a submodule under source/vendor/h264bitstream.
# Upstream ships its own standalone CMake build (autotools-style, generates
# config.h, adds install/test targets); we deliberately do not use it and
# compile the needed sources into a static library instead. Upstream license:
# LGPL-2.1-or-later, which is compatible with this project's GPLv3+.

set(H264BS_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/h264bitstream)

# The actual library is h264_stream.c + h264_sei.c + h264_nal.c (see upstream
# Makefile.am). The generated h264_slice_data.c / h264_avcc.c are leftover
# generator inputs that do not compile standalone - we do not need them.
add_library(h264bitstream STATIC
    ${H264BS_SOURCE_DIR}/h264_stream.c
    ${H264BS_SOURCE_DIR}/h264_sei.c
    ${H264BS_SOURCE_DIR}/h264_nal.c)

target_include_directories(h264bitstream PUBLIC ${H264BS_SOURCE_DIR})

# Vendor code is not held to our warning level.
target_compile_options(h264bitstream PRIVATE -w)
