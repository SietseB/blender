/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgpencil
 */

#pragma once

#include "GPU_shader_shared_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

void gpencil_ondine_render_set_data(struct Object *ob, const float mat[4][4]);
void gpencil_ondine_render_set_zdepth(struct Object *ob);

void gpencil_ondine_set_render_data(struct Object *ob, const float4x4 mat);
void gpencil_ondine_set_zdepth(struct Object *ob);
bool gpencil_ondine_render_init(struct bContext *C);
void gpencil_ondine_set_unique_stroke_seeds(struct bContext *C);

#ifdef __cplusplus
}
#endif
