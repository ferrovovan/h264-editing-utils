# Copyright (C) 2026 h264-editing-utils contributors
#
# SPDX short: GPLv3+ — see /COPYING/COPYING at the project root.

# Vendored Unity (MIT) — foreign code, compiled without warnings.
add_library(unity STATIC
    unit/vendor/unity/unity.c)
target_include_directories(unity PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/unit/vendor/unity)
target_compile_options(unity PRIVATE -w)

# One test_<module>.c file = one ctest test (unit_<module>). New test files
# picked up automatically by CONFIGURE_DEPENDS, no CMake edit needed.
file(GLOB UNIT_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_*.c)
foreach(test_src ${UNIT_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} unity h264common)
    add_test(NAME unit_${test_name} COMMAND ${test_name})
endforeach()

# Mutation fuzz of h264-cut (label "fuzz"; slow, ~2-3 min).
add_test(NAME fuzz COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/fuzz.sh $<TARGET_FILE:h264-cut>)
set_tests_properties(fuzz PROPERTIES LABELS fuzz)
