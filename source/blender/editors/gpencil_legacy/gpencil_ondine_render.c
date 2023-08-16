/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. */

/** \file
 * \ingroup edgpencil
 * Operators for Ondine watercolor Grease Pencil.
 */

#include "BKE_context.h"

#include "WM_api.h"
#include "WM_types.h"

/* Set render data on GP object */
void gpencil_ondine_render_set_data(Object *ob, const float mat[4][4])
{
  gpencil_ondine_set_render_data(ob, mat);
}
