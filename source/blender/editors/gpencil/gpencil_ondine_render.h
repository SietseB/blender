/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgpencil
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void gpencil_ondine_render_set_data(Object *ob);
void gpencil_ondine_render_set_zdepth(Object *ob);

void gpencil_ondine_set_render_data(Object *ob);
void gpencil_ondine_set_zdepth(Object *ob);
bool gpencil_ondine_render_init(bContext *C);
void gpencil_ondine_set_unique_stroke_seeds(bContext *C);

#ifdef __cplusplus
}
#endif
