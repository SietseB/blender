/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2017 Blender Foundation. */

/** \file
 * \ingroup modifiers
 */

#include <stdio.h>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_gpencil_legacy_types.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_gpencil_geom_legacy.h"
#include "BKE_gpencil_legacy.h"
#include "BKE_gpencil_modifier_legacy.h"
#include "BKE_lib_query.h"
#include "BKE_modifier.h"
#include "BKE_screen.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "ED_gpencil_legacy.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"

#include "MOD_gpencil_legacy_modifiertypes.h"
#include "MOD_gpencil_legacy_ui_common.h"
#include "MOD_gpencil_legacy_util.h"

static void init_data(GpencilModifierData *md)
{
  MorphTargetsGpencilModifierData *gpmd = (MorphTargetsGpencilModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(gpmd, modifier));

  MEMCPY_STRUCT_AFTER(gpmd, DNA_struct_default_get(MorphTargetsGpencilModifierData), modifier);
}

static void copy_data(const GpencilModifierData *md, GpencilModifierData *target)
{
  MorphTargetsGpencilModifierData *gmd = (MorphTargetsGpencilModifierData *)md;
  MorphTargetsGpencilModifierData *tgmd = (MorphTargetsGpencilModifierData *)target;

  BKE_gpencil_modifier_copydata_generic(md, target);

  tgmd->factor = gmd->factor;
  tgmd->index_edited = gmd->index_edited;
  tgmd->gpd_base = gmd->gpd_base;
  tgmd->base_layers = gmd->base_layers;
}

/* Change stroke points by active morph targets. */
static void morph_strokes(GpencilModifierData *md,
                          Object *ob,
                          bGPDlayer *gpl,
                          bGPDframe *gpf,
                          float *mt_factor,
                          const int mt_count)
{
  bGPDsmorph *gpsm_lookup[GPENCIL_MORPH_TARGETS_MAX];
  MorphTargetsGpencilModifierData *mmd = (MorphTargetsGpencilModifierData *)md;

  /* Vertex group filter. */
  const int def_nr = BKE_object_defgroup_name_index(ob, mmd->vgname);
  bool vg_is_inverted = (mmd->flag & GP_MORPHTARGETS_INVERT_VGROUP) != 0;

  /* Morph all strokes in frame. */
  LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
    bool morphed = false;
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
                                        mmd->flag & GP_MORPHTARGETS_INVERT_MATERIAL))
    {
      continue;
    }

    /* Create lookup table of morphs in stroke. */
    for (int i = 0; i < mt_count; i++) {
      gpsm_lookup[i] = nullptr;
    }
    LISTBASE_FOREACH (bGPDsmorph *, gpsm, &gps->morphs) {
      gpsm_lookup[gpsm->morph_target_nr] = gpsm;
    }

    /* Iterate all morphs in stroke. */
    for (int mi = 0; mi < mt_count; mi++) {
      bGPDsmorph *gpsm = gpsm_lookup[mi];
      if (gpsm == nullptr) {
        continue;
      }

      /* Get factor. */
      float factor = mt_factor[gpsm->morph_target_nr];

      /* Skip morphs with factor 0. */
      if (factor == 0.0f) {
        continue;
      }

      /* Skip morphs with unequal number of points. */
      if (gps->totpoints != gpsm->tot_point_deltas) {
        continue;
      }

      /* Apply fill color morph to stroke. */
      bGPDspoint *pt1;
      float vecb[3], vecm[3], color_delta[4];
      float mat[3][3];
      morphed = true;

      copy_v4_v4(color_delta, gpsm->fill_color_delta);
      mul_v4_fl(color_delta, factor);
      add_v4_v4(gps->vert_color_fill, color_delta);
      clamp_v4(gps->vert_color_fill, 0.0f, 1.0f);

      /* Continue when there aren't morphed stroke points. */
      if (gpsm->point_deltas == nullptr) {
        continue;
      }

      /* Apply stroke point morphs. */
      for (int i = 0; i < gps->totpoints; i++) {
        /* Verify point is part of vertex group. */
        MDeformVert *dvert = gps->dvert != nullptr ? &gps->dvert[i] : nullptr;
        float weight = get_modifier_point_weight(dvert, vg_is_inverted, def_nr);
        if (weight <= 0.0f) {
          continue;
        }
        factor *= weight;

        bGPDspoint *pt = &gps->points[i];
        bGPDspoint_delta *pd = &gpsm->point_deltas[i];

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
            vecb[0] = 1.0f;
            vecb[1] = 0.0f;
            vecb[2] = 0.0f;
            mul_m3_v3(mat, vecb);
            normalize_v3(vecb);
          }
          mul_v3_v3fl(vecm, vecb, pd->distance * fabs(factor));
          if (factor < 0.0f) {
            negate_v3(vecm);
          }
          add_v3_v3(&pt->x, vecm);
        }

        pt->pressure += pd->pressure * factor;
        clamp_f(pt->pressure, 0.0f, FLT_MAX);
        pt->strength += pd->strength * factor;
        clamp_f(pt->strength, 0.0f, 1.0f);
        copy_v4_v4(color_delta, pd->vert_color);
        mul_v4_fl(color_delta, factor);
        add_v4_v4(pt->vert_color, color_delta);
        clamp_v4(pt->vert_color, 0.0f, 1.0f);
      }
    }

    if (morphed) {
      /* Mark stroke for geometry update. */
      gps->runtime.flag |= GP_STROKE_UPDATE_GEOMETRY;
    }
  }
}

static void morph_object(GpencilModifierData *md,
                         Depsgraph *depsgraph,
                         Scene *scene,
                         Object *ob,
                         const bool update_deltas)
{
  float mt_factor[GPENCIL_MORPH_TARGETS_MAX];
  bGPDlmorph *gplm_lookup[GPENCIL_MORPH_TARGETS_MAX];
  MorphTargetsGpencilModifierData *mmd = (MorphTargetsGpencilModifierData *)md;
  bGPdata *gpd = (bGPdata *)ob->data;

  /* Update stroke deltas on the fly when a morph target is edited. */
  if (update_deltas && mmd->gpd_base != nullptr) {
    ED_gpencil_morph_target_update_stroke_deltas(mmd, depsgraph, scene, ob);
  }

  /* Create lookup table for morph target values by index. */
  int mt_count = 0;
  LISTBASE_FOREACH (bGPDmorph_target *, gpmt, &gpd->morph_targets) {
    /* Fully apply a morph when currently edited. */
    if (mt_count == mmd->index_edited) {
      mt_factor[mt_count] = 1.0f;
    }
    /* Don't apply morph when muted. */
    else if ((gpmt->flag & GP_MORPH_TARGET_MUTE) != 0) {
      mt_factor[mt_count] = 0.0f;
    }
    else {
      mt_factor[mt_count] = gpmt->value * mmd->factor;
    }
    mt_count++;
  }
  if (mt_count == 0) {
    return;
  }

  /* Get direction for applying layer order morphs. */
  bool apply_forward = false;
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    LISTBASE_FOREACH (bGPDlmorph *, gplm, &gpl->morphs) {
      if (gplm->order_applied != 0) {
        apply_forward = true;
        break;
      }
    }
    if (apply_forward) {
      break;
    }
  }

  /* Apply layer order morphs. */
  bGPDlayer *gpl_sibl;
  bGPDlayer *gpl_order = static_cast<bGPDlayer *>((apply_forward) ? gpd->layers.first :
                                                                    gpd->layers.last);
  for (; gpl_order; gpl_order = gpl_sibl) {
    gpl_sibl = (apply_forward) ? gpl_order->next : gpl_order->prev;

    if (BLI_listbase_is_empty(&gpl_order->morphs)) {
      continue;
    }

    /* Layer filter. */
    if (!is_layer_affected_by_modifier(mmd->layername,
                                       mmd->layer_pass,
                                       gpl_order,
                                       mmd->flag & GP_MORPHTARGETS_INVERT_LAYER,
                                       mmd->flag & GP_MORPHTARGETS_INVERT_LAYERPASS))
    {
      continue;
    }

    /* Create lookup table of morphs in layer. */
    for (int i = 0; i < mt_count; i++) {
      gplm_lookup[i] = nullptr;
    }
    LISTBASE_FOREACH (bGPDlmorph *, gplm, &gpl_order->morphs) {
      gplm_lookup[gplm->morph_target_nr] = gplm;
    }

    /* Get layer order morphs. */
    bGPDmorph_target *gpmt = static_cast<bGPDmorph_target *>(gpd->morph_targets.first);
    for (int mi = 0; mi < mt_count; mi++, gpmt = gpmt->next) {
      bGPDlmorph *gplm = gplm_lookup[mi];
      if ((gplm == nullptr) || (gplm->order == 0) || (mi == mmd->index_edited) ||
          ((gpmt->flag & GP_MORPH_TARGET_MUTE) != 0))
      {
        continue;
      }

      /* Check flipping point of layer order morph. */
      const float factor = mt_factor[gplm->morph_target_nr];
      const bool change_order =
          (((gpmt->layer_order_compare == GP_MORPH_TARGET_COMPARE_GREATER_THAN) &&
            (factor > gpmt->layer_order_value)) ||
           ((gpmt->layer_order_compare == GP_MORPH_TARGET_COMPARE_LESS_THAN) &&
            (factor < gpmt->layer_order_value)));
      int dir = 0;
      int order_delta = 0;
      if ((gplm->order_applied == 0) && change_order) {
        /* Apply layer order morph. */
        dir = 1;
        order_delta = gplm->order;
      }
      else if ((gplm->order_applied != 0) && (!change_order)) {
        /* Revert layer order morph. */
        dir = -1;
        order_delta = -1 * gplm->order_applied;
      }

      /* Move layer. */
      if (dir != 0) {
        if (!BLI_listbase_link_move(&gpd->layers, gpl_order, order_delta)) {
          BLI_remlink(&gpd->layers, gpl_order);
          if (order_delta < 0) {
            BLI_addhead(&gpd->layers, gpl_order);
          }
          else {
            BLI_addtail(&gpd->layers, gpl_order);
          }
        }

        gplm->order_applied = (dir == -1) ? 0 : order_delta;
        gpd->runtime.morph_target_flag |= GP_MORPH_TARGET_MORPHED_LAYER_ORDER;
      }
    }
  }

  /* Morph all layers (transform and opacity). */
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    /* Layer filter. */
    if (!is_layer_affected_by_modifier(mmd->layername,
                                       mmd->layer_pass,
                                       gpl,
                                       mmd->flag & GP_MORPHTARGETS_INVERT_LAYER,
                                       mmd->flag & GP_MORPHTARGETS_INVERT_LAYERPASS))
    {
      continue;
    }

    /* Get frame. */
    bGPDframe *gpf = BKE_gpencil_frame_retime_get(depsgraph, scene, ob, gpl);
    if (gpf == nullptr) {
      continue;
    }

    /* Create lookup table of morphs in layer. */
    for (int i = 0; i < mt_count; i++) {
      gplm_lookup[i] = nullptr;
    }
    LISTBASE_FOREACH (bGPDlmorph *, gplm, &gpl->morphs) {
      gplm_lookup[gplm->morph_target_nr] = gplm;
    }

    /* Init original transform data, otherwise we get 'morph on morph on morph'. */
    bGPDlayer *gpl_orig = (gpl->runtime.gpl_orig) ? gpl->runtime.gpl_orig : gpl;
    copy_v3_v3(gpl->location, gpl_orig->location);
    copy_v3_v3(gpl->rotation, gpl_orig->rotation);
    copy_v3_v3(gpl->scale, gpl_orig->scale);
    gpl->opacity = gpl_orig->opacity;

    /* Apply layer morphs. */
    for (int mi = 0; mi < mt_count; mi++) {
      bGPDlmorph *gplm = gplm_lookup[mi];
      if ((gplm == nullptr) || (mi == mmd->index_edited)) {
        continue;
      }
      float factor = mt_factor[gplm->morph_target_nr];
      if (factor == 0.0f) {
        continue;
      }

      /* Apply delta transformation and opacity. */
      for (int i = 0; i < 3; i++) {
        gpl->location[i] += gplm->location[i] * factor;
        gpl->rotation[i] += gplm->rotation[i] * factor;
        gpl->scale[i] += gplm->scale[i] * factor;
      }
      gpl->opacity += gplm->opacity * factor;
    }
    gpl->opacity = clamp_f(gpl->opacity, 0.0f, 1.0f);
    loc_eul_size_to_mat4(gpl->layer_mat, gpl->location, gpl->rotation, gpl->scale);
    invert_m4_m4(gpl->layer_invmat, gpl->layer_mat);

    /* Morph all strokes in frame. */
    morph_strokes(md, ob, gpl, gpf, mt_factor, mt_count);
  }
}

static void bake_modifier(struct Main * /*bmain*/,
                          Depsgraph *depsgraph,
                          GpencilModifierData *md,
                          Object *ob)
{
  Scene *scene = DEG_get_evaluated_scene(depsgraph);
  morph_object(md, depsgraph, scene, ob, false);
}

/* Generic "generate_strokes" callback */
static void generate_strokes(GpencilModifierData *md, Depsgraph *depsgraph, Object *ob)
{
  Scene *scene = DEG_get_evaluated_scene(depsgraph);
  morph_object(md, depsgraph, scene, ob, true);
}

static void foreach_ID_link(GpencilModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  MorphTargetsGpencilModifierData *mmd = (MorphTargetsGpencilModifierData *)md;

  walk(userData, ob, (ID **)&mmd->material, IDWALK_CB_USER);
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, nullptr);

  uiLayoutSetPropSep(layout, true);

  uiLayout *col = uiLayoutColumn(layout, true);
  uiItemR(col, ptr, "factor", UI_ITEM_NONE, nullptr, ICON_NONE);

  gpencil_modifier_panel_end(layout, ptr);
}

static void mask_panel_draw(const bContext * /*C*/, Panel *panel)
{
  gpencil_modifier_masking_panel_draw(panel, true, true);
}

static void panel_register(ARegionType *region_type)
{
  PanelType *panel_type = gpencil_modifier_panel_register(
      region_type, eGpencilModifierType_MorphTargets, panel_draw);
  gpencil_modifier_subpanel_register(
      region_type, "mask", "Influence", nullptr, mask_panel_draw, panel_type);
}

GpencilModifierTypeInfo modifierType_Gpencil_MorphTargets = {
    /*name*/ N_("Morph Targets"),
    /*struct_name*/ "MorphTargetsGpencilModifierData",
    /*struct_size*/ sizeof(MorphTargetsGpencilModifierData),
    /*type*/ eGpencilModifierTypeType_Gpencil,
    /*flags*/ eGpencilModifierTypeFlag_SupportsEditmode,

    /*copy_data*/ copy_data,

    /*deform_stroke*/ nullptr,
    /*generate_strokes*/ generate_strokes,
    /*bake_modifier*/ bake_modifier,
    /*remap_time*/ nullptr,

    /*init_data*/ init_data,
    /*free_data*/ nullptr,
    /*is_disabled*/ nullptr,
    /*update_depsgraph*/ nullptr,
    /*depends_on_time*/ nullptr,
    /*foreach_ID_link*/ foreach_ID_link,
    /*foreach_tex_link*/ nullptr,
    /*panel_register*/ panel_register,
};
