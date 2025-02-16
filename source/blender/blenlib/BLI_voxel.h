/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

/** Calculate the index number of a voxel, given x/y/z integer coords and resolution vector. */
#define BLI_VOXEL_INDEX(x, y, z, res) \
  ((int64_t)(x) + (int64_t)(y) * (int64_t)(res)[0] + \
   (int64_t)(z) * (int64_t)(res)[0] * (int64_t)(res)[1])

/* All input coordinates must be in bounding box 0.0 - 1.0. */

float BLI_voxel_sample_trilinear(const float *data, const int res[3], const float co[3]);
