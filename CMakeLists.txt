# Copyright (C) 2026 h264-editing-utils contributors
#
# This file is part of h264-editing-utils.
#
# h264-editing-utils is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# h264-editing-utils is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with h264-editing-utils.  If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.16)
project(h264-editing-utils C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra)

option(WITH_SMART "Поддержка --smart (нужны x264, libavcodec, libavutil)" ON)

# ASAN+UBSAN for OUR targets only (vendor h264bitstream and Unity stay
# uninstrumented). The link flags propagate transitively to every consumer
# binary, so the sanitizer runtime is linked into h264-cut and the unit tests.
option(ENABLE_SANITIZERS "Build h264common and h264-cut with ASAN+UBSAN" OFF)

enable_testing()

add_subdirectory(source)

if(ENABLE_SANITIZERS)
    foreach(san_tgt h264common h264-cut avcc2annexb annexb2avcc)
        target_compile_options(${san_tgt} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    endforeach()
    target_link_options(h264common INTERFACE -fsanitize=address,undefined)
    target_link_options(h264-cut PRIVATE -fsanitize=address,undefined)
    target_link_options(avcc2annexb PRIVATE -fsanitize=address,undefined)
    target_link_options(annexb2avcc PRIVATE -fsanitize=address,undefined)
endif()

add_subdirectory(tests)

add_test(NAME integration
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run.sh $<TARGET_FILE:h264-cut>)
