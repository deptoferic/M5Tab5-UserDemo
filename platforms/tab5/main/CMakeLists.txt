file(GLOB_RECURSE APP_LAYER_SRCS
    ../../../app/*.c
    ../../../app/*.cc
    ../../../app/*.cpp
)

set(APP_LAYER_INCS
    ../../../app
)

file(GLOB_RECURSE MY_HAL_SRCS
    ./hal/*.c
    ./hal/*.cc
    ./hal/*.cpp
)

# `shared` is the Tab5 Sensor Hub's Schema A component, reached via
# EXTRA_COMPONENT_DIRS in the project CMakeLists.
#
# Do NOT add `REQUIRES shared` here. `main` implicitly depends on every
# component in the build, so shared's include directory is inherited already.
# Specifying REQUIRES explicitly REPLACES that implicit dependency, and the
# build immediately loses mooncake.h, lvgl_cpp/* and everything else. Tried
# 2026-08-26; it fails loudly and is easy to mistake for a shared/ problem.
idf_component_register(SRCS "app_main.cpp" "link_probe.c" "link/link_tab5.c" "console_tab5.cpp" ${APP_LAYER_SRCS} ${MY_HAL_SRCS}
                    INCLUDE_DIRS "." ${APP_LAYER_INCS}
                    EMBED_TXTFILES "../audio/canon_in_d.mp3" "../audio/startup_sfx.mp3" "../audio/shutdown_sfx.mp3")
