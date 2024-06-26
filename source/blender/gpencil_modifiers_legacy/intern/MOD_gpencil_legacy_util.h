/* SPDX-FileCopyrightText: Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct Depsgraph;
struct GpencilModifierData;
struct MDeformVert;
struct Material;
struct Object;
struct bGPDlayer;
struct bGPDframe;
struct bGPDstroke;

/**
 * Verify if valid layer, material and pass index.
 */
bool is_stroke_affected_by_modifier(struct Object *ob,
                                    char *mlayername,
                                    struct Material *material,
                                    int mpassindex,
                                    int gpl_passindex,
                                    int minpoints,
                                    bGPDlayer *gpl,
                                    bGPDstroke *gps,
                                    bool inv1,
                                    bool inv2,
                                    bool inv3,
                                    bool inv4);

/**
 * Verify if valid layer and pass index.
 */
bool is_layer_affected_by_modifier(char *mlayername,
                                   const int gpl_passindex,
                                   struct bGPDlayer *gpl,
                                   const bool inv1,
                                   const bool inv2);

/**
 * Verify if valid vertex group *and return weight.
 */
float get_modifier_point_weight(struct MDeformVert *dvert, bool inverse, int def_nr);
/**
 * Generic bake function for deform_stroke.
 */
typedef void (*gpBakeCb)(struct GpencilModifierData *md_,
                         struct Depsgraph *depsgraph_,
                         struct Object *ob_,
                         struct bGPDlayer *gpl_,
                         struct bGPDframe *gpf_,
                         struct bGPDstroke *gps_);

void generic_bake_deform_stroke(struct Depsgraph *depsgraph,
                                struct GpencilModifierData *md,
                                struct Object *ob,
                                bool retime,
                                gpBakeCb bake_cb);

#ifdef __cplusplus
}
#endif
