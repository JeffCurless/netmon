cmake_minimum_required(VERSION 3.18)   # 3.18: TARGET_DIRECTORY below

# ---------------------------------------------------------------------------
# Board name aliases — mirrors picoOS/CMakeLists.txt.
# Required here because we call add_subdirectory(picoOS/src) directly,
# bypassing picoOS's root.
# ---------------------------------------------------------------------------
if(PICO_BOARD STREQUAL "picow")
    set(PICO_BOARD "pico_w" CACHE STRING "" FORCE)
elseif(PICO_BOARD STREQUAL "pico2w")
    set(PICO_BOARD "pico2_w" CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Optimise for size by default (-Os) — mirrors picoOS/CMakeLists.txt, which is
# bypassed here.  Without it the SDK picks Release (-O3), which inlines and
# unrolls loops for much more flash.  Must come before the SDK import, which
# applies its own default.  Override with -DCMAKE_BUILD_TYPE=Release.
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING
        "Choose the type of build, options are: 'Debug', 'Release', 'MinSizeRel', 'RelWithDebInfo'.")
endif()

include(pico_sdk_import.cmake)

project(myproject C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ---------------------------------------------------------------------------
# Application injection
#
# Use a project-owned option so the user can pass -DMYPROJECT_DEMO=ON to
# substitute the picoOS built-in demo apps for hardware bring-up.
#
# IMPORTANT: set PICOOS_INCLUDE_DEMO_APPS and PICOOS_APP_SOURCES as normal
# CMake variables (not CACHE variables) before add_subdirectory().  Passing
# them as -D flags would create cache entries that conflict with picoOS's own
# option() declarations inside the subdirectory.
# ---------------------------------------------------------------------------
option(MYPROJECT_DEMO "Build with picoOS demo apps instead of project apps" OFF)

if(MYPROJECT_DEMO)
    set(PICOOS_INCLUDE_DEMO_APPS ON)
else()
    set(PICOOS_INCLUDE_DEMO_APPS OFF)
    # Absolute paths are required — picoOS/src resolves relative paths against
    # its own source directory, not the project root.
    set(PICOOS_APP_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/app_table.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/netmon.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/oui.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/oui_data.c
    )
    set(PICOOS_APP_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/apps
    )
endif()

# ---------------------------------------------------------------------------
# NetMon version — read from VERSION file at the repo root.
# ---------------------------------------------------------------------------
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NETMON_VERSION)
string(STRIP "${NETMON_VERSION}" NETMON_VERSION)
add_compile_definitions(NETMON_VERSION="${NETMON_VERSION}")

add_subdirectory(picoOS/src)

# ---------------------------------------------------------------------------
# Hold the netmon sources to the picoOS warning set (-Wall -Wextra ... -Werror).
# TARGET_DIRECTORY picoos is required: the picoos target lives in picoOS/src,
# and without it this call is silently ignored.
#
# -Wno-format-truncation: netmon clips text to fit display rows on purpose
# (snprintf into fixed 20-byte row buffers), so that warning is all noise.
# ---------------------------------------------------------------------------
if(PICOOS_APP_SOURCES)
    set(NETMON_WARNINGS ${PICOOS_WARNINGS} -Wno-format-truncation)
    set_source_files_properties(${PICOOS_APP_SOURCES}
        TARGET_DIRECTORY picoos
        PROPERTIES COMPILE_OPTIONS "${NETMON_WARNINGS}")
endif()
