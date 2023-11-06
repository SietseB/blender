# SPDX-FileCopyrightText: 2020 Blender Authors
#
# SPDX-License-Identifier: BSD-3-Clause

# - Find cairo library
# Find the cairo include and library
# This module defines
#  CAIRO_INCLUDE_DIRS, where to find cairolib.h, Set when
#                    CAIRO is found.
#  CAIRO_LIBRARIES, libraries to link against to use CAIRO.
#  CAIRO_ROOT_DIR, The base directory to search for CAIRO.
#                This can also be an environment variable.
#  CAIRO_FOUND, If false, do not try to use CAIRO.
#
# also defined, but not for general use are
#  CAIRO_LIBRARY, where to find the CAIRO library.

# If `CAIRO_ROOT_DIR` was defined in the environment, use it.
if(DEFINED CAIRO_ROOT_DIR)
  # Pass.
elseif(DEFINED ENV{CAIRO_ROOT_DIR})
  set(CAIRO_ROOT_DIR $ENV{CAIRO_ROOT_DIR})
else()
  set(CAIRO_ROOT_DIR "")
endif()

set(_cairo_SEARCH_DIRS
  ${CAIRO_ROOT_DIR}
  /opt/lib/cairo
  /usr/include
  /usr/local/include
)

find_path(CAIRO_INCLUDE_DIR
  NAMES
    cairo.h
  HINTS
    ${_cairo_SEARCH_DIRS}
  PATH_SUFFIXES
    include
)

find_library(CAIRO_LIBRARY
  NAMES
    cairo
  HINTS
    ${_cairo_SEARCH_DIRS}
  PATH_SUFFIXES
    lib64 lib
  )

# handle the QUIETLY and REQUIRED arguments and set CAIRO_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Cairo DEFAULT_MSG
    CAIRO_LIBRARY CAIRO_INCLUDE_DIR)

if(CAIRO_FOUND)
  set(CAIRO_LIBRARIES ${CAIRO_LIBRARY})
  set(CAIRO_INCLUDE_DIRS ${CAIRO_INCLUDE_DIR})
endif()

mark_as_advanced(
  CAIRO_INCLUDE_DIR
  CAIRO_LIBRARY
)
