/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. */

/** \file
 * \ingroup edgpencil
 * Operators for Ondine watercolor Grease Pencil.
 */

#include "BKE_context.h"

#include "WM_api.h"
#include "WM_types.h"

#include "gpencil_ondine_render.h"

/* Set render data on GP object */
void gpencil_ondine_render_set_data(Object *ob)
{
  gpencil_ondine_set_render_data(ob);
}

/* Set z-depth on GP object */
void gpencil_ondine_render_set_zdepth(Object *ob)
{
  gpencil_ondine_set_zdepth(ob);
}

/* Init Ondine watercolor rendering for current frame */
static int gpencil_ondine_set_unique_stroke_seeds_exec(bContext *C, wmOperator *op)
{
  gpencil_ondine_set_unique_stroke_seeds(C);
  return OPERATOR_FINISHED;
}

/* Operator definition: ondine_set_unique_stroke_seeds */
void GPENCIL_OT_ondine_set_unique_stroke_seeds(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Set Unique Stroke Seeds";
  ot->idname = "GPENCIL_OT_ondine_set_unique_stroke_seeds";
  ot->description = "Set unique stroke seeds in each frame for Ondine watercolor rendering";

  /* api callbacks */
  ot->exec = gpencil_ondine_set_unique_stroke_seeds_exec;
}

/* Init Ondine watercolor rendering for current frame */
static int gpencil_ondine_render_init_exec(bContext *C, wmOperator *op)
{
  bool success = gpencil_ondine_render_init(C);
  if (!success) {
    return OPERATOR_CANCELLED;
  }

  return OPERATOR_FINISHED;
}

/* Operator definition: ondine_render_init */
void GPENCIL_OT_ondine_render_init(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Init Ondine rendering";
  ot->idname = "GPENCIL_OT_ondine_render_init";
  ot->description = "Initialize Ondine watercolor rendering for current frame";

  /* api callbacks */
  ot->exec = gpencil_ondine_render_init_exec;
}
