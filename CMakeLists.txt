cmake_minimum_required(VERSION 3.13)

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
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/myapp.c
    )
    set(PICOOS_APP_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/apps
    )
endif()

add_subdirectory(picoOS/src)
