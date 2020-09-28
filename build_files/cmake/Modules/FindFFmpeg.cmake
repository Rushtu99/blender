# - Find FFmpeg library and includes.
# Set FFMPEG_FIND_COMPONENTS to the canonical names of the libraries
# before using the package.
# This module defines
#  FFMPEG_INCLUDE_DIRS, where to find libavcodec/ac3_parser.h.
#  FFMPEG_LIBRARIES, libraries to link against to use FFmpeg.
#  FFMPEG_ROOT_DIR, The base directory to search for FFmpeg.
#                        This can also be an environment variable.
#  FFMPEG_FOUND, If false, do not try to use FFmpeg.
#  FFMPEG_<COMPONENT>_LIBRARY, the given individual component libraries.
#=============================================================================
# Copyright 2020 Blender Foundation.
#
# Distributed under the OSI-approved BSD 3-Clause License,
# see accompanying file BSD-3-Clause-license.txt for details.
#=============================================================================

# If FFMPEG_ROOT_DIR was defined in the environment, use it.
if(NOT FFMPEG_ROOT_DIR AND NOT $ENV{FFMPEG_ROOT_DIR} STREQUAL "")
  set(FFMPEG_ROOT_DIR $ENV{FFMPEG_ROOT_DIR})
endif()

set(_ffmpeg_SEARCH_DIRS
  ${FFMPEG_ROOT_DIR}
  /opt/lib/ffmpeg
)

if(NOT FFMPEG_FIND_COMPONENTS)
  set(FFMPEG_FIND_COMPONENTS
    # List taken from http://ffmpeg.org/download.html#build-mac
    avcodec
    avdevice
    avfilter
    avformat
    avutil
  )
endif()

find_path(_ffmpeg_INCLUDE_DIR
  NAMES
    libavcodec/ac3_parser.h
  HINTS
    ${_ffmpeg_SEARCH_DIRS}
  PATH_SUFFIXES
    include
)

set(_ffmpeg_LIBRARIES)
foreach(_component ${FFMPEG_FIND_COMPONENTS})
  string(TOUPPER ${_component} _upper_COMPONENT)
  find_library(FFMPEG_${_upper_COMPONENT}_LIBRARY
    NAMES
      ${_upper_COMPONENT}
    HINTS
      ${LIBDIR}/ffmpeg
    PATH_SUFFIXES
      lib64 lib
  )
  if(NOT FFMPEG_${_upper_COMPONENT}_LIBRARY)
    message(WARNING "Could not find FFpeg ${_upper_COMPONENT}.")
  endif()
  list(APPEND _ffmpeg_LIBRARIES ${FFMPEG_${_upper_COMPONENT}_LIBRARY})
  mark_as_advanced(FFMPEG_${_upper_COMPONENT}_LIBRARY)
endforeach()

# handle the QUIETLY and REQUIRED arguments and set FFMPEG_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg DEFAULT_MSG
    _ffmpeg_LIBRARIES _ffmpeg_INCLUDE_DIR)

IF(FFMPEG_FOUND)
  set(FFMPEG_LIBRARIES ${_ffmpeg_LIBRARIES})
  set(FFMPEG_INCLUDE_DIRS ${_ffmpeg_INCLUDE_DIR})
ENDIF(FFMPEG_FOUND)

mark_as_advanced(
  FFMPEG_INCLUDE_DIR
)

unset(_ffmpeg_SEARCH_DIRS)
unset(_ffmpeg_LIBRARIES)
unset(_ffmpeg_INCLUDE_DIR)
