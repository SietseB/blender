# SPDX-FileCopyrightText: 2012-2022 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(CAIRO_EXTRA_ARGS
)

if(WIN32)
  ExternalProject_Add(external_cairo
    URL file://${PACKAGE_DIR}/${CAIRO_FILE}
    DOWNLOAD_DIR ${DOWNLOAD_DIR}
    URL_HASH ${CAIRO_HASH_TYPE}=${CAIRO_HASH}
    PREFIX ${BUILD_DIR}/cairo
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${LIBDIR}/cairo ${CAIRO_EXTRA_ARGS}
    INSTALL_DIR ${LIBDIR}/cairo
  )
endif()
