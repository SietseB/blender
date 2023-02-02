/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2017 Blender Foundation. */

/** \file
 * \ingroup modifiers
 */

#include <stdio.h>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_lib_query.h"
#include "BKE_modifier.h"
#include "BKE_screen.h"

#include "DEG_depsgraph.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"

#include "MOD_gpencil_modifiertypes.h"
#include "MOD_gpencil_ui_common.h"
#include "MOD_gpencil_util.h"

typedef struct MorphTargetsData {
  Object *ob;
  float factor[GPENCIL_MORPH_TARGETS_MAX];
} MorphTargetsData;

static void initData(GpencilModifierData *md)
{
  MorphTargetsGpencilModifierData *gpmd = (MorphTargetsGpencilModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(gpmd, modifier));

  MEMCPY_STRUCT_AFTER(gpmd, DNA_struct_default_get(MorphTargetsGpencilModifierData), modifier);
}

static void copyData(const GpencilModifierData *md, GpencilModifierData *target)
{
  MorphTargetsGpencilModifierData *gmd = (MorphTargetsGpencilModifierData *)md;
  MorphTargetsGpencilModifierData *tgmd = (MorphTargetsGpencilModifierData *)target;

  BKE_gpencil_modifier_copydata_generic(md, target);

  tgmd->factor = gmd->factor;
}

/* Change stroke points by active morph targets. */
static void deformStroke(GpencilModifierData *md,
                         Depsgraph *UNUSED(depsgraph),
                         Object *ob,
                         bGPDlayer *gpl,
                         bGPDframe *UNUSED(gpf),
                         bGPDstroke *gps)
{
  MorphTargetsGpencilModifierData *mmd = (MorphTargetsGpencilModifierData *)md;

  if (!is_stroke_affected_by_modifier(ob,
                                      mmd->layername,
                                      mmd->material,
                                      mmd->pass_index,
                                      mmd->layer_pass,
                                      1,
                                      gpl,
                                      gps,
                                      mmd->flag & GP_MORPHTARGETS_INVERT_LAYER,
                                      mmd->flag & GP_MORPHTARGETS_INVERT_PASS,
                                      mmd->flag & GP_MORPHTARGETS_INVERT_LAYERPASS,
                                      mmd->flag & GP_MORPHTARGETS_INVERT_MATERIAL)) {
    return;
  }

  /* Vertex group filter. */
  const int def_nr = BKE_object_defgroup_name_index(ob, mmd->vgname);
  bool vg_is_inverted = (mmd->flag & GP_MORPHTARGETS_INVERT_VGROUP) != 0;

  /* Iterate all morphs in stroke. */
  for (bGPDsmorph *gpsm = gps->morphs.first; gpsm; gpsm = gpsm->next) {
    /* Get morph target factor. */
    float factor = mmd->mt_factor[gpsm->morph_target_nr];

    /* Skip morphs with factor 0. */
    if (factor == 0.0f) {
      continue;
    }

    /* Skip morphs with unequal number of points. */
    if (gps->totpoints != gpsm->tot_point_deltas) {
      continue;
    }

    /* Apply morph to stroke. */
    bGPDspoint *pt1;
    float vecb[3], vecm[3];
    float mat[3][3];

    for (int i = 0; i < gps->totpoints; i++) {
      /* Verify point is part of vertex group. */
      MDeformVert *dvert = gps->dvert != NULL ? &gps->dvert[i] : NULL;
      float weight = get_modifier_point_weight(dvert, vg_is_inverted, def_nr);
      if (weight <= 0.0f) {
        continue;
      }
      factor *= weight;

      bGPDspoint *pt = &gps->points[i];
      bGPDspoint_delta *pd = &gpsm->point_deltas[i];
      float color_delta[3];

      /* Convert quaternion rotation to point delta. */
      if (pd->distance > 0.0f) {
        quat_to_mat3(mat, pd->rot_quat);
        if (i < (gps->totpoints - 1)) {
          pt1 = &gps->points[i + 1];
          sub_v3_v3v3(vecb, &pt1->x, &pt->x);
          mul_m3_v3(mat, vecb);
          normalize_v3(vecb);
        }
        else if (gps->totpoints == 1) {
          zero_v3(vecb);
          vecb[0] = 1.0f;
          mul_m3_v3(mat, vecb);
          normalize_v3(vecb);
        }
        mul_v3_v3fl(vecm, vecb, pd->distance * factor);
        add_v3_v3(&pt->x, vecm);
      }

      pt->pressure += pd->pressure * factor;
      clamp_f(pt->pressure, 0.0f, FLT_MAX);
      pt->strength += pd->strength * factor;
      clamp_f(pt->strength, 0.0f, 1.0f);
      copy_v3_v3(color_delta, pd->vert_color);
      mul_v3_fl(color_delta, factor);
      add_v3_v3(pt->vert_color, color_delta);
      clamp_v3(pt->vert_color, 0.0f, 1.0f);
    }
  }
}

static void bakeModifier(struct Main *UNUSED(bmain),
                         Depsgraph *depsgraph,
                         GpencilModifierData *md,
                         Object *ob)
{
  generic_bake_deform_stroke(depsgraph, md, ob, false, deformStroke);
}

static void foreachIDLink(GpencilModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  MorphTargetsGpencilModifierData *mmd = (MorphTargetsGpencilModifierData *)md;

  walk(userData, ob, (ID **)&mmd->material, IDWALK_CB_USER);
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, NULL);

  uiLayoutSetPropSep(layout, true);

  uiLayout *col = uiLayoutColumn(layout, true);
  uiItemR(col, ptr, "factor", 0, NULL, ICON_NONE);

  gpencil_modifier_panel_end(layout, ptr);
}

static void mask_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  gpencil_modifier_masking_panel_draw(panel, true, true);
}

static void panelRegister(ARegionType *region_type)
{
  PanelType *panel_type = gpencil_modifier_panel_register(
      region_type, eGpencilModifierType_MorphTargets, panel_draw);
  gpencil_modifier_subpanel_register(
      region_type, "mask", "Influence", NULL, mask_panel_draw, panel_type);
}

GpencilModifierTypeInfo modifierType_Gpencil_MorphTargets = {
    /*name*/ N_("Morph Targets"),
    /*structName*/ "MorphTargetsGpencilModifierData",
    /*structSize*/ sizeof(MorphTargetsGpencilModifierData),
    /*type*/ eGpencilModifierTypeType_Gpencil,
    /*flags*/ eGpencilModifierTypeFlag_SupportsEditmode,

    /*copyData*/ copyData,

    /*deformStroke*/ deformStroke,
    /*generateStrokes*/ NULL,
    /*bakeModifier*/ bakeModifier,
    /*remapTime*/ NULL,

    /*initData*/ initData,
    /*freeData*/ NULL,
    /*isDisabled*/ NULL,
    /*updateDepsgraph*/ NULL,
    /*dependsOnTime*/ NULL,
    /*foreachIDLink*/ foreachIDLink,
    /*foreachTexLink*/ NULL,
    /*panelRegister*/ panelRegister,
};
