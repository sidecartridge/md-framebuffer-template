# Set minimum required version of CMake
cmake_minimum_required(VERSION 3.26)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# if PICOTOOL_OVERRIDE_DIR system environment variable is set,
# then use that as the folder for the picotool executable
if (DEFINED ENV{PICOTOOL_OVERRIDE_DIR})
    message("PICOTOOL_OVERRIDE_DIR env var is set to '$ENV{PICOTOOL_OVERRIDE_DIR}'")
    add_executable(picotool IMPORTED GLOBAL)
    set_property(TARGET picotool PROPERTY IMPORTED_LOCATION $ENV{PICOTOOL_OVERRIDE_DIR}/picotool)
    # check the picotool version:
    execute_process(COMMAND $ENV{PICOTOOL_OVERRIDE_DIR}/picotool version
                    OUTPUT_VARIABLE PICOTOOL_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX MATCH "^picotool v${picotool_VERSION_REQUIRED}" PICOTOOL_VERSION_MATCH ${PICOTOOL_VERSION})
    if (NOT PICOTOOL_VERSION_MATCH)
        message("picotool version response was: ${PICOTOOL_VERSION}")
        message(FATAL_ERROR "PICOTOOL_OVERRIDE_DIR is set to '$ENV{PICOTOOL_OVERRIDE_DIR}', but the version of picotool found is not ${picotool_VERSION_REQUIRED}")
    else()
        message("picotool version response was: ${PICOTOOL_VERSION}")
    endif()
endif ()

# If not specified, use pico_w as default board type
set(BOARD_TYPE $ENV{BOARD_TYPE})
if(NOT BOARD_TYPE)
    set(BOARD_TYPE "pico_w")
endif()
set(PICO_BOARD ${BOARD_TYPE})

# Determine board type macros
if("${BOARD_TYPE}" STREQUAL "pico_w")
    add_compile_definitions(BOARD_TYPE_PICO_W=1 BOARD_TYPE_PICO=0)
else()
    message(FATAL_ERROR "Unknown BOARD_TYPE: ${BOARD_TYPE}")
endif()

# Resolve bundled Pico dependencies relative to the repo root so configure
# works regardless of the active build directory.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
set(DEFAULT_FATFS_SDK_PATH "${REPO_ROOT}/fatfs-sdk")
set(DEFAULT_PICO_SDK_PATH "${REPO_ROOT}/pico-sdk")
set(DEFAULT_PICO_EXTRAS_PATH "${REPO_ROOT}/pico-extras")

if(NOT DEFINED ENV{FATFS_SDK_PATH} OR "$ENV{FATFS_SDK_PATH}" STREQUAL "")
    set(ENV{FATFS_SDK_PATH} "${DEFAULT_FATFS_SDK_PATH}")
endif()
set(FATFS_SDK_PATH "$ENV{FATFS_SDK_PATH}" CACHE PATH "Path to the FATFS SDK" FORCE)
get_filename_component(FATFS_SDK_PATH "${FATFS_SDK_PATH}" REALPATH BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(FATFS_SDK_PATH "${FATFS_SDK_PATH}" CACHE PATH "Path to the FATFS SDK" FORCE)
set(ENV{FATFS_SDK_PATH} "${FATFS_SDK_PATH}")
message("FATFS_SDK_PATH: ${FATFS_SDK_PATH}")

if(NOT DEFINED ENV{PICO_SDK_PATH} OR "$ENV{PICO_SDK_PATH}" STREQUAL "")
    set(ENV{PICO_SDK_PATH} "${DEFAULT_PICO_SDK_PATH}")
endif()
set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK" FORCE)
get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK" FORCE)
set(ENV{PICO_SDK_PATH} "${PICO_SDK_PATH}")
message("PICO_SDK_PATH: ${PICO_SDK_PATH}")

if(NOT DEFINED ENV{PICO_EXTRAS_PATH} OR "$ENV{PICO_EXTRAS_PATH}" STREQUAL "")
    set(ENV{PICO_EXTRAS_PATH} "${DEFAULT_PICO_EXTRAS_PATH}")
endif()
set(PICO_EXTRAS_PATH "$ENV{PICO_EXTRAS_PATH}" CACHE PATH "Path to the PICO EXTRAS" FORCE)
get_filename_component(PICO_EXTRAS_PATH "${PICO_EXTRAS_PATH}" REALPATH BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(PICO_EXTRAS_PATH "${PICO_EXTRAS_PATH}" CACHE PATH "Path to the PICO EXTRAS" FORCE)
set(ENV{PICO_EXTRAS_PATH} "${PICO_EXTRAS_PATH}")
message("PICO_EXTRAS_PATH: ${PICO_EXTRAS_PATH}")

# Include build functions from Pico SDK
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
include(${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)

# Set name of project (as PROJECT_NAME) and C/C   standards
project(rp C CXX)
set(CMAKE_C_STANDARD 11)

# malloc must return NULL on failure instead of panicking.
#
# This MUST come before pico_sdk_init(): the check that panics lives in the
# SDK's own malloc.c, and pico_sdk_init() adds the SDK as a subdirectory, which
# inherits compile definitions as they stand at that moment. Defined further
# down, it would compile our sources with it and leave the SDK's malloc.c
# untouched, which looks right and does nothing.
#
# The SDK default (PICO_MALLOC_PANIC 1) turns a failed allocation into
# panic("Out of memory"), which defeats code written to handle NULL. FatFs's
# directory creation is the case that matters: it asks for a buffer as large
# as a cluster (32 KB on a large FAT32 card) and halves the request until it
# fits, which it can only do if malloc is allowed to say no. Every allocation
# in this firmware checks its result.
if (NOT DEFINED ENV{PICO_MALLOC_PANIC})
    set(ENV{PICO_MALLOC_PANIC} "0")
endif()
add_compile_definitions(PICO_MALLOC_PANIC=$ENV{PICO_MALLOC_PANIC})

# The app's profile (include/profile.h): PROFILE_50FPS, the default, or
# PROFILE_25FPS, from the environment (APP_PROFILE=PROFILE_25FPS ./build.sh).
if (DEFINED ENV{APP_PROFILE})
    add_compile_definitions(APP_PROFILE=$ENV{APP_PROFILE})
endif()

# Creates a pico-sdk subdirectory in our project for the libraries
pico_sdk_init()

add_subdirectory(settings)

# Tell CMake where to find other source code
add_subdirectory(${FATFS_SDK_PATH}/src build)
  
# Tell CMake where to find the executable source file
add_executable(${PROJECT_NAME} 
        main.c
)

pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/romemul.pio)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/commemul.pio)

target_sources(${PROJECT_NAME} PRIVATE
        aconfig.c
        audio.c
        commemul.c
        emul.c
        fb.c
        fb_blit.c
        fb_blit_asm.S
        fb_chunked.c
        fb_chunked_asm.S
        fb_font.c
        gconfig.c
        ikbd.c
        ikbd_demux.c
        hw_config.c
        palette.c
        demo_menu.c
        demo_parallax.c
        demo_3d.c
        demo_sprites.c
        demo_cojorotozoom.c
        demo_input.c
        demo_games.c
        game_kit.c
        game_arena.c
        game_zap.c
        reset.c
        romemul.c
        sdcard.c
        select.c
        st_session.c
        settings/settings.c)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(${PROJECT_NAME})

# Link libraries required for the project
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${LINK_LIBRARIES}        # External or additional libraries passed as variables
    hardware_flash           # Flash memory access
    hardware_interp          # SIO interpolators (cojorotozoom texel addressing)
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico                # FATFS library
    pico_stdlib              # Core functionality
    pico_multicore          # Multicore support
    settings                 # Custom settings library
)


# Link custom memmap with reserved memory for ROMs
set_target_properties(${PROJECT_NAME} PROPERTIES
        PICO_TARGET_LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/memmap_rp.ld
)

# Make our project-local FatFs config (rp/src/ff/ffconf.h) win over the
# fatfs-sdk submodule's default copy. fatfs sources do `#include "ffconf.h"`
# which is resolved against the include path; placing this BEFORE PRIVATE
# guarantees our override is found first, so the submodule stays pristine
# and no on-the-fly sed patching is needed.
target_include_directories(${PROJECT_NAME} BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/ff)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/..)


# Use reduced space for ROMs
pico_set_binary_type(${PROJECT_NAME} rp)

# Fetch the values from the environment variables
set(RELEASE_TYPE $ENV{RELEASE_TYPE})
set(RELEASE_VERSION $ENV{RELEASE_VERSION})
set(RELEASE_DATE $ENV{RELEASE_DATE})
set(_DEBUG $ENV{DEBUG_MODE})

# If the environment variables are not set, use default values
if(NOT RELEASE_VERSION)

    # Check if RELEASE_TYPE is set and not empty, otherwise use default "FINAL"
    if(NOT RELEASE_TYPE OR RELEASE_TYPE STREQUAL "")
        set(RELEASE_TYPE "final")
    endif()

    # Determine the file name based on RELEASE_TYPE
    if(RELEASE_TYPE STREQUAL "final")
        set(VERSION_FILE_NAME "version.txt")
    else()
        set(VERSION_FILE_NAME "version-${RELEASE_TYPE}.txt")
    endif()
    
    # Remove newline character if present
    string(STRIP "${RELEASE_VERSION}" RELEASE_VERSION)

    # In case version.txt is empty or does not exist, use a default version
    if("${RELEASE_VERSION}" STREQUAL "")
        set(RELEASE_VERSION "v0.0.1dev")
    endif()
endif()

if(NOT RELEASE_DATE)
        string(TIMESTAMP CURRENT_DATE_TIME "%Y-%m-%d %H:%M:%S")
        set(RELEASE_DATE ${CURRENT_DATE_TIME})
endif()

# Default _DEBUG to 0 when the env var isn't set at all. `if (NOT _DEBUG)`
# would also fire for the string "0" (CMake treats "0" as falsy), silently
# turning a CMakePresets release preset into a debug build. Mirrors the
# pattern used in rp2-atarist-rpikb's CMakeLists.txt.
if ("${_DEBUG}" STREQUAL "")
        set(_DEBUG 0)
endif()

# Debug outputs
pico_enable_stdio_usb(${PROJECT_NAME} 0)
# Workaround to disable USB output in release builds
if(${_DEBUG} STREQUAL "0")
    pico_enable_stdio_uart(${PROJECT_NAME} 0)
else()
    pico_enable_stdio_uart(${PROJECT_NAME} 1)
    # 921,600 baud, not the SDK's 115,200: DPRINTF blocks on the UART, and at
    # 115,200 the boot trace delayed the cartridge image by about 0.3 s, so on
    # a power-on the ST's TOS checked the cartridge before it was there and
    # booted to GEM. The UART clock error at this rate is 0.02 %. Set the
    # serial terminal (tools/dev/console.py) to 921600.
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        PICO_DEFAULT_UART_BAUD_RATE=921600)
endif()

# Print the variables on screen
message("RELEASE_TYPE: " ${RELEASE_TYPE})
message("RELEASE_VERSION: " ${RELEASE_VERSION})
message("RELEASE_DATE: " ${RELEASE_DATE})
message("DEBUG_MODE: " ${_DEBUG})

# Pass these values to the C compiler
add_definitions(-DRELEASE_TYPE="${RELEASE_TYPE}")
add_definitions(-DRELEASE_VERSION="${RELEASE_VERSION}")
add_definitions(-DRELEASE_DATE="${RELEASE_DATE}")
# The SDK's binary info carries a build date (picotool info shows it). It is
# __DATE__ by default, the day of the compile, so two builds of one commit
# made on different days differed; it is RELEASE_DATE instead.
add_definitions(-DPICO_PROGRAM_BUILD_DATE="${RELEASE_DATE}")

# Pass the APP_UUID_KEY to the C compiler
if(DEFINED ENV{APP_UUID_KEY} AND NOT "$ENV{APP_UUID_KEY}" STREQUAL "")
    message(STATUS "Using APP_UUID_KEY from environment: $ENV{APP_UUID_KEY}")
    add_definitions(-DCURRENT_APP_UUID_KEY=\"$ENV{APP_UUID_KEY}\")
else()
    message(STATUS "Using default UUID")
    add_definitions(-DCURRENT_APP_UUID_KEY=\"44444444-4444-4444-8444-444444444444\")
endif()

# Pass the _DEBUG flag to the C compiler
add_definitions(-D_DEBUG=${_DEBUG})

# Device/Computer type
add_definitions(-DDISPLAY_ATARIST)

# We don't modify the flash in Core 1
add_definitions(-DPICO_FLASH_ASSUME_CORE0_SAFE=1)

# PICO_HEAP_SIZE is the heap the link guarantees: a build whose static data
# leaves less fails with "region RAM overflowed" instead of booting with a heap
# too small for the settings store and FatFs. Measured on the device: the boot
# grows the heap to 13.2 KB (newlib's malloc grows it in 4 KB steps), and with
# 9 KB the first settings allocation failed and the app jumped to Booster.
target_compile_definitions(${PROJECT_NAME} PRIVATE PICO_HEAP_SIZE=0x8000)

# Core 0's stack takes all of SCRATCH_Y (4 KB) instead of the SDK's 2 KB, and
# a guard at its bottom turns an overflow into a HardFault instead of a silent
# walk through SCRATCH_X into core 1's stack. Core 1's stack follows
# PICO_STACK_SIZE too: all of SCRATCH_X, with its own guard. The deepest static
# path from main, the boot's SD error trace, is 1,576 bytes (2,192 while the
# FATFS object sat in emul_start's frame), plus newlib's printf, which the
# analysis cannot see; core 1's jobs use well under 200 bytes.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    PICO_STACK_SIZE=0x1000
    PICO_USE_STACK_GUARDS=1)


# Remove unused data. The ELF keeps its symbol table: it never reaches the
# .uf2 (byte-identical either way), and the SWD tools find the cartridge
# window, the build ID and the counters by symbol.
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--gc-sections")

# build_id.h and build_id.c (RELEASE_BUILD_ID and the release_build_id symbol,
# the git commit) are regenerated on every build; see build_id.cmake. They are
# rewritten only when the ID changes, so only their users recompile. The symbol
# is kept through --gc-sections so the SWD tools can read it from flash. A
# debug build's ID ends in +debug, a build of another CMake type than
# Release carries that type and a 25 fps build (APP_PROFILE) +25fps, so builds
# of one tree cannot be taken for each other on the device.
set(BUILD_ID_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/build_id)
add_custom_target(build_id
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_CURRENT_LIST_DIR}
            -DOUT_DIR=${BUILD_ID_DIR} -DDEBUG_BUILD=${_DEBUG}
            -DCMAKE_TYPE=${CMAKE_BUILD_TYPE} -DAPP_PROFILE=$ENV{APP_PROFILE}
            -P ${CMAKE_CURRENT_LIST_DIR}/build_id.cmake
    BYPRODUCTS ${BUILD_ID_DIR}/build_id.h ${BUILD_ID_DIR}/build_id.c
    COMMENT "Checking the build ID")
add_dependencies(${PROJECT_NAME} build_id)
target_sources(${PROJECT_NAME} PRIVATE ${BUILD_ID_DIR}/build_id.c)
set_source_files_properties(${BUILD_ID_DIR}/build_id.c PROPERTIES GENERATED TRUE)
target_include_directories(${PROJECT_NAME} PRIVATE ${BUILD_ID_DIR})
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--undefined=release_build_id")

# Enable clang-tidy (you need to have clang-tidy installed on your system)
find_program(CLANG_TIDY_EXE NAMES clang-tidy)

if(CLANG_TIDY_EXE)
    set(CMAKE_C_CLANG_TIDY "${CLANG_TIDY_EXE}")
    message(STATUS "Found clang-tidy: ${CLANG_TIDY_EXE}")
else()
    message(WARNING "clang-tidy not found, skipping linting")
endif()

# Find clang-format executable
find_program(CLANG_FORMAT_BIN NAMES clang-format)

if(CLANG_FORMAT_BIN)
    message(STATUS "Found clang-format: ${CLANG_FORMAT_BIN}")
    message(STATUS "Linting in the source dir: ${CMAKE_SOURCE_DIR}")

    # Add a custom target to run clang-format on all source files
    file(GLOB_RECURSE ALL_CXX_SOURCE_FILES
        "${CMAKE_SOURCE_DIR}/*.c"
        "${CMAKE_SOURCE_DIR}/include/*.h"
    )

    add_custom_target(clang-format
        COMMAND ${CLANG_FORMAT_BIN}
        -i
        -style=file
        ${ALL_CXX_SOURCE_FILES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/../..
        COMMENT "Running clang-format on the source code..."
        VERBATIM
    )

else()
    message(WARNING "clang-format not found!")
endif()
