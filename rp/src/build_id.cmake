# Writes build_id.h (RELEASE_BUILD_ID) and build_id.c (the same string as the
# release_build_id symbol in flash, which tools read over SWD) with the git
# commit the firmware was built from. Run on every build (not only at configure
# time), so an incremental build never reports a stale ID.
#
#   <sha7>                     clean tree
#   <sha7>-dirty.<diff7>       uncommitted changes; <diff7> hashes the diff,
#                              so different changes give different IDs and the
#                              same tree always gives the same ID. The generated
#                              target_firmware.h does not count: build.sh
#                              regenerates it on every build with the build's
#                              date in its header, and the m68k sources it is
#                              made from do count
#   $ENV{RELEASE_BUILD_ID}     when set (for example a source copy outside git);
#                              cut to 21 characters
#   nogit                      not a git checkout
#
# A build of another CMake type than Release (-DCMAKE_TYPE=MinSizeRel, for
# RP_CMAKE_BUILD_TYPE) appends its type (+minsizerel; CMake Debug is
# +cmakedebug), an HTTPS build (-DHTTPS_BUILD=1) +https and a debug build
# (-DDEBUG_BUILD=1) +debug to any of these, so the ID in flash tells the builds
# of one tree apart.
#
# Inputs: -DSRC_DIR=<rp/src> -DOUT_DIR=<folder for build_id.h and build_id.c>
#         -DDEBUG_BUILD=<0|1> -DHTTPS_BUILD=<0|1> -DCMAKE_TYPE=<CMake build type>

if(DEFINED ENV{RELEASE_BUILD_ID} AND NOT "$ENV{RELEASE_BUILD_ID}" STREQUAL "")
  set(BUILD_ID "$ENV{RELEASE_BUILD_ID}")
else()
  execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY "${SRC_DIR}"
    OUTPUT_VARIABLE SHA
    RESULT_VARIABLE SHA_RESULT
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT SHA_RESULT EQUAL 0)
    set(BUILD_ID "nogit")
  else()
    # Submodules are excluded: rp/build.sh checks them out at pinned tags.
    execute_process(
      COMMAND git diff HEAD --binary --ignore-submodules --
              . ":(exclude)rp/src/include/target_firmware.h"
      WORKING_DIRECTORY "${SRC_DIR}/../.."
      OUTPUT_VARIABLE DIFF
      ERROR_QUIET)
    if("${DIFF}" STREQUAL "")
      set(BUILD_ID "${SHA}")
    else()
      string(SHA1 DIFF_HASH "${DIFF}")
      string(SUBSTRING "${DIFF_HASH}" 0 7 DIFF_HASH)
      set(BUILD_ID "${SHA}-dirty.${DIFF_HASH}")
    endif()
  endif()
endif()

# Longest shape is 21 characters (<sha7>-dirty.<diff7>), 33 with +https+debug,
# 48 with +relwithdebinfo too.
string(SUBSTRING "${BUILD_ID}" 0 21 BUILD_ID)
string(TOLOWER "${CMAKE_TYPE}" CMAKE_TYPE_TAG)
if(CMAKE_TYPE_TAG STREQUAL "debug")
  set(CMAKE_TYPE_TAG "cmakedebug")
endif()
if(NOT CMAKE_TYPE_TAG STREQUAL "" AND NOT CMAKE_TYPE_TAG STREQUAL "release")
  set(BUILD_ID "${BUILD_ID}+${CMAKE_TYPE_TAG}")
endif()
if(HTTPS_BUILD)
  set(BUILD_ID "${BUILD_ID}+https")
endif()
if(DEBUG_BUILD)
  set(BUILD_ID "${BUILD_ID}+debug")
endif()

function(write_if_changed path content)
  if(EXISTS "${path}")
    file(READ "${path}" old)
  else()
    set(old "")
  endif()
  if(NOT "${old}" STREQUAL "${content}")
    file(WRITE "${path}" "${content}")
  endif()
endfunction()

write_if_changed("${OUT_DIR}/build_id.h"
  "#pragma once\n#define RELEASE_BUILD_ID \"${BUILD_ID}\"\nextern const char release_build_id[];\n")
write_if_changed("${OUT_DIR}/build_id.c"
  "#include \"build_id.h\"\nconst char release_build_id[] = RELEASE_BUILD_ID;\n")
