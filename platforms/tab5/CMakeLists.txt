# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ../../../shared is the Tab5 Sensor Hub's Schema A component
# (shared/link_schema.h) — the wire contract shared with the satellite firmware.
# Both binaries MUST compile against the same header; see that project's
# CLAUDE.md §5 and docs/schema_a.md.
#
# It must go here rather than being passed with -D: this is a plain set(), which
# shadows the cache variable a -DEXTRA_COMPONENT_DIRS=... would populate. That
# was confirmed by test — the reconfigure succeeded and the component was still
# not visible.
set(EXTRA_COMPONENT_DIRS
    "../../dependencies"
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared"
) 

project(m5stack_tab5)
