/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. */

/** \file
 * \ingroup edgpencil
 *
 * Operators for dealing with GP morph targets.
 */

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_ghash.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_string_utils.hh"

#include "BLT_translation.hh"

#include "DNA_anim_types.h"
#include "DNA_gpencil_legacy_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BKE_anim_data.hh"
#include "BKE_animsys.h"
#include "BKE_context.hh"
#include "BKE_gpencil_legacy.h"
#include "BKE_gpencil_modifier_legacy.h"
#include "BKE_report.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "ED_gpencil_legacy.hh"
#include "ED_object.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "BLF_api.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "gpencil_intern.hh"

/* Temporary morph operation data `op->customdata`. */
struct tGPDmorph {
  /** Current active gp object. */
  struct Object *ob;
  /** Area where painting originated. */
  struct ScrArea *area;
  /** Region where painting originated. */
  struct ARegion *region;
  /** 3D viewport draw handler. */
  void *draw_handle;
  /** Height of tool header region in viewport. */
  int header_height;
  /** Width of the N-panel. */
  int npanel_width;

  /** Base GP data-block. */
  struct bGPdata *gpd_base;
  /** Morph target GP data-block. */
  struct bGPdata *gpd_morph;
  /** Active morph target. */
  bGPDmorph_target *active_gpmt;
  /** Active morph target index. */
  int active_index;
};

/* State: is a morph target being edited? */
bool in_edit_mode = false;

/* ************************************************ */
/* Morph Target Operators */

/* ******************* Add New Morph Target ************************ */
static void gpencil_morph_target_increase_number(bGPdata *gpd, const int index)
{
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    LISTBASE_FOREACH (bGPDlmorph *, gplm, &gpl->morphs) {
      if (gplm->morph_target_nr >= index) {
        gplm->morph_target_nr++;
      }
    }

    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        LISTBASE_FOREACH (bGPDsmorph *, gpsm, &gps->morphs) {
          if (gpsm->morph_target_nr >= index) {
            gpsm->morph_target_nr++;
          }
        }
      }
    }
  }
}

static int gpencil_morph_target_add_exec(bContext *C, wmOperator *op)
{
  bGPdata *gpd = nullptr;

  Object *ob = CTX_data_active_object(C);
  if ((ob != nullptr) && (ob->type == OB_GPENCIL_LEGACY)) {
    /* Check maximum number of morph targets. */
    gpd = (bGPdata *)ob->data;
    if (BLI_listbase_count(&gpd->morph_targets) >= GPENCIL_MORPH_TARGETS_MAX) {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  "Maximum number of morph targets reached (%d)",
                  GPENCIL_MORPH_TARGETS_MAX);
      return OPERATOR_CANCELLED;
    }

    /* Get name. */
    bool name_given = false;
    PropertyRNA *prop;
    char name[128];
    prop = RNA_struct_find_property(op->ptr, "name");
    if (RNA_property_is_set(op->ptr, prop)) {
      RNA_property_string_get(op->ptr, prop, name);
      name_given = true;
    }
    else {
      strcpy(name, "Morph");
    }

    /* Create morph target and set default values. */
    bGPDmorph_target *gpmt_act = BKE_gpencil_morph_target_active_get(gpd);
    bGPDmorph_target *gpmt = nullptr;
    gpmt = static_cast<bGPDmorph_target *>(
        MEM_callocN(sizeof(bGPDmorph_target), "bGPDmorph_target"));
    if (gpmt_act != nullptr) {
      BLI_insertlinkafter(&gpd->morph_targets, gpmt_act, gpmt);
    }
    else {
      BLI_addtail(&gpd->morph_targets, gpmt);
    }

    gpmt->range_min = 0.0f;
    gpmt->range_max = 1.0f;
    gpmt->value = 0.0f;
    gpmt->layer_order_compare = GP_MORPH_TARGET_COMPARE_GREATER_THAN;
    gpmt->layer_order_value = 0.5f;

    /* Copy values of currently active morph target. */
    if (gpmt_act != nullptr) {
      if (!name_given) {
        strcpy(name, gpmt_act->name);
      }
      gpmt->range_min = gpmt_act->range_min;
      gpmt->range_max = gpmt_act->range_max;
      gpmt->layer_order_compare = gpmt_act->layer_order_compare;
      gpmt->layer_order_value = gpmt->layer_order_value;

      /* Renumber morph target index of layer and stroke morphs. */
      if (gpmt->next != nullptr) {
        int index = BLI_findindex(&gpd->morph_targets, gpmt);
        gpencil_morph_target_increase_number(gpd, index);
      }
    }

    /* Auto-name. */
    BLI_strncpy(gpmt->name, DATA_(name), sizeof(gpmt->name));
    BLI_uniquename(&gpd->morph_targets,
                   gpmt,
                   DATA_("Morph"),
                   '.',
                   offsetof(bGPDmorph_target, name),
                   sizeof(gpmt->name));

    /* Set active. */
    BKE_gpencil_morph_target_active_set(gpd, gpmt);

    /* Add morph targets modifier automatically when there isn't one. */
    GpencilModifierData *md = BKE_gpencil_modifiers_findby_type(ob,
                                                                eGpencilModifierType_MorphTargets);
    if (md == nullptr) {
      Main *bmain = CTX_data_main(C);
      Scene *scene = CTX_data_scene(C);

      md = blender::ed::object::gpencil_modifier_add(
          op->reports, bmain, scene, ob, "Morph Targets", eGpencilModifierType_MorphTargets);
      if (md == nullptr) {
        BKE_report(op->reports, RPT_ERROR, "Unable to add a Morph Targets modifier to object");
      }
    }
  }

  /* notifiers */
  if (gpd) {
    DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  }
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, nullptr);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, nullptr);

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_morph_target_add(wmOperatorType *ot)
{

  /* Identifiers. */
  ot->name = "Add New Morph Target";
  ot->idname = "GPENCIL_OT_morph_target_add";
  ot->description = "Add new morph target for the active data-block";

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Operator properties. */
  PropertyRNA *prop;
  prop = RNA_def_string(
      ot->srna, "name", nullptr, MAX_NAME, "Name", "Name of the newly added morph target");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  ot->prop = prop;

  /* Callbacks. */
  ot->exec = gpencil_morph_target_add_exec;
  ot->poll = gpencil_add_poll;
}

/* ******************* Remove Morph Target ************************ */
static int gpencil_morph_target_remove_exec(bContext *C, wmOperator *op)
{
  bGPdata *gpd = ED_gpencil_data_get_active(C);
  bGPDmorph_target *gpmt = BKE_gpencil_morph_target_active_get(gpd);

  /* Delete morph target data from all strokes and layers
   * and lower the indexes higher than the morph target index
   * to be removed. */
  int index = BLI_findindex(&gpd->morph_targets, gpmt);
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    LISTBASE_FOREACH_MUTABLE (bGPDlmorph *, gplm, &gpl->morphs) {
      if (gplm->morph_target_nr == index) {
        BLI_freelinkN(&gpl->morphs, gplm);
      }
      else if (gplm->morph_target_nr > index) {
        gplm->morph_target_nr--;
      }
    }

    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        LISTBASE_FOREACH_MUTABLE (bGPDsmorph *, gpsm, &gps->morphs) {
          if (gpsm->morph_target_nr == index) {
            if (gpsm->point_deltas != nullptr) {
              MEM_freeN(gpsm->point_deltas);
            }
            BLI_freelinkN(&gps->morphs, gpsm);
          }
          else if (gpsm->morph_target_nr > index) {
            gpsm->morph_target_nr--;
          }
        }
      }
    }
  }

  /* Update anim data. */
  char name_esc[sizeof(gpmt->name) * 2];
  char rna_path[sizeof(gpmt->name) * 2 + 32];
  BLI_str_escape(name_esc, gpmt->name, sizeof(name_esc));
  BLI_snprintf(rna_path, sizeof(rna_path), "morph_targets[\"%s\"]", name_esc);
  BKE_animdata_fix_paths_remove(&gpd->id, rna_path);

  /* Set new active morph target. */
  if (gpmt->next != nullptr) {
    BKE_gpencil_morph_target_active_set(gpd, gpmt->next);
  }
  else if (gpmt->prev != nullptr) {
    BKE_gpencil_morph_target_active_set(gpd, gpmt->prev);
  }

  /* Delete morph target. */
  BLI_freelinkN(&gpd->morph_targets, gpmt);

  /* When no morph targets left, remove all morph target modifiers automatically. */
  if (BLI_listbase_is_empty(&gpd->morph_targets)) {
    Object *ob = CTX_data_active_object(C);
    Main *bmain = CTX_data_main(C);

    LISTBASE_FOREACH_MUTABLE (GpencilModifierData *, md, &ob->greasepencil_modifiers) {
      if (md->type != eGpencilModifierType_MorphTargets) {
        continue;
      }
      blender::ed::object::gpencil_modifier_remove(op->reports, bmain, ob, md);
    }
  }

  /* notifiers */
  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, nullptr);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, nullptr);

  return OPERATOR_FINISHED;
}

static bool gpencil_morph_target_active_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if ((ob == nullptr) || (ob->type != OB_GPENCIL_LEGACY)) {
    return false;
  }
  bGPdata *gpd = (bGPdata *)ob->data;
  bGPDmorph_target *gpmt = BKE_gpencil_morph_target_active_get(gpd);

  return (gpmt != nullptr);
}

void GPENCIL_OT_morph_target_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove Morph Target";
  ot->idname = "GPENCIL_OT_morph_target_remove";
  ot->description = "Remove active Grease Pencil morph target";

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* callbacks */
  ot->exec = gpencil_morph_target_remove_exec;
  ot->poll = gpencil_morph_target_active_poll;
}

/* ******************* Move Morph Target ************************ */
static int gpencil_morph_target_move_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  bGPdata *gpd = (bGPdata *)ob->data;
  bGPDmorph_target *gpmt = BKE_gpencil_morph_target_active_get(gpd);

  int dir = RNA_enum_get(op->ptr, "direction");
  int old_index = BLI_findindex(&gpd->morph_targets, gpmt);
  int new_index = old_index + dir;
  if ((new_index < 0) || (new_index >= BLI_listbase_count(&gpd->morph_targets))) {
    return OPERATOR_CANCELLED;
  }

  /* Move morph target in list. */
  BLI_listbase_link_move(&gpd->morph_targets, gpmt, dir);

  /* Swap morph target indexes of layer and stroke morphs. */
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    LISTBASE_FOREACH (bGPDlmorph *, gplm, &gpl->morphs) {
      if (gplm->morph_target_nr == old_index) {
        gplm->morph_target_nr = new_index;
      }
      else if (gplm->morph_target_nr == new_index) {
        gplm->morph_target_nr = old_index;
      }
    }

    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        LISTBASE_FOREACH (bGPDsmorph *, gpsm, &gps->morphs) {
          if (gpsm->morph_target_nr == old_index) {
            gpsm->morph_target_nr = new_index;
          }
          else if (gpsm->morph_target_nr == new_index) {
            gpsm->morph_target_nr = old_index;
          }
        }
      }
    }
  }

  /* Notifiers. */
  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, nullptr);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, nullptr);

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_morph_target_move(wmOperatorType *ot)
{
  static const EnumPropertyItem morph_target_order_move[] = {
      {-1, "UP", 0, "Up", ""},
      {1, "DOWN", 0, "Down", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* identifiers */
  ot->name = "Move Morph Target";
  ot->idname = "GPENCIL_OT_morph_target_move";
  ot->description = "Move the active morph target up/down in the list";

  /* api callbacks */
  ot->poll = gpencil_morph_target_active_poll;
  ot->exec = gpencil_morph_target_move_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "direction",
               morph_target_order_move,
               0,
               "Direction",
               "Direction to move the active morph target towards");
}

/* ******************* Edit Morph Target ************************ */
bool ED_gpencil_morph_target_in_edit_mode()
{
  return in_edit_mode;
}

static void gpencil_morph_target_edit_exit(bContext *C, wmOperator *op)
{
  tGPDmorph *tgpm = static_cast<tGPDmorph *>(op->customdata);

  /* Clean up temp data. */
  if (tgpm) {
    /* Remove viewport draw handler. */
    if (tgpm->draw_handle) {
      ED_region_draw_cb_exit(tgpm->region->type, tgpm->draw_handle);
    }

    /* Clear edit state of morph target in modifiers. */
    LISTBASE_FOREACH (GpencilModifierData *, md, &tgpm->ob->greasepencil_modifiers) {
      if (md->type == eGpencilModifierType_MorphTargets) {
        MorphTargetsGpencilModifierData *mmd = (MorphTargetsGpencilModifierData *)md;
        mmd->index_edited = -1;
        mmd->gpd_base = nullptr;
        if (mmd->base_layers != nullptr) {
          BLI_ghash_free(mmd->base_layers, nullptr, nullptr);
        }
        mmd->base_layers = nullptr;
      }
    }

    /* Remove base GP objects. */
    LISTBASE_FOREACH_MUTABLE (bGPDlayer *, gpl, &tgpm->gpd_base->layers) {
      LISTBASE_FOREACH_MUTABLE (bGPDframe *, gpf, &gpl->frames) {
        LISTBASE_FOREACH_MUTABLE (bGPDstroke *, gps, &gpf->strokes) {
          MEM_freeN(gps->points);
          BLI_freelinkN(&gpf->strokes, gps);
        }
        BLI_freelinkN(&gpl->frames, gpf);
      }
      BLI_freelinkN(&tgpm->gpd_base->layers, gpl);
    }
    MEM_freeN(tgpm->gpd_base);

    /* Update morphed GP object. */
    DEG_id_tag_update(&tgpm->gpd_morph->id,
                      ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_SYNC_TO_EVAL);
    WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, nullptr);

    MEM_freeN(tgpm);
  }

  /* Clear 'in morph edit mode' flag. */
  in_edit_mode = false;

  op->customdata = nullptr;
}

static void gpencil_morph_target_edit_draw(const bContext * /*C*/, ARegion *region, void *arg)
{
  tGPDmorph *tgpm = (tGPDmorph *)arg;

  /* Draw only in the region set by the operator. */
  if (region != tgpm->region) {
    return;
  }

  /* Draw rectangle outline. */
  float half_line_w = 3.0f * UI_SCALE_FAC;
  rcti *rect = &region->winrct;
  float color[4];
  UI_GetThemeColor4fv(TH_SELECT_ACTIVE, color);
  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4fv(color);
  GPU_line_width(2 * half_line_w);
  imm_draw_box_wire_2d(pos,
                       half_line_w,
                       half_line_w,
                       rect->xmax - rect->xmin - tgpm->npanel_width - half_line_w,
                       rect->ymax - rect->ymin - tgpm->header_height - 2);
  immUnbindProgram();

  /* Draw text. */
  const int font_id = BLF_default();
  const uiStyle *style = UI_style_get();
  BLF_size(font_id, style->widget.points * UI_SCALE_FAC);
  BLF_color4fv(font_id, color);
  BLF_enable(font_id, BLF_SHADOW);
  BLF_shadow(font_id, FontShadowType::Outline, blender::float4{0.0f, 0.0f, 0.0f, 0.7f});
  BLF_shadow_offset(font_id, 1, -1);

  const char *text;
  text = TIP_("Editing Morph Target");
  float x = (rect->xmax - rect->xmin - tgpm->npanel_width) * 0.5f -
            BLF_width(font_id, text, strlen(text)) * 0.5f;
  float y = rect->ymax - rect->ymin - tgpm->header_height - style->widget.points * UI_SCALE_FAC -
            half_line_w * 3;
  BLF_position(font_id, x, y, 0);
  BLF_draw(font_id, text, strlen(text));
  BLF_disable(font_id, BLF_SHADOW);
}

static constexpr float EPSILON = 0.00001f;

static int gpencil_morph_target_create_stroke_deltas(bGPDframe *gpf_base,
                                                     bGPDframe *gpf_morph,
                                                     const int active_morph_index)
{
  float vecb[3], vecm[3];
  bGPDsmorph smorph;
  bGPDstroke *gps_base = static_cast<bGPDstroke *>(gpf_base->strokes.first);
  bGPDstroke *gps_morph = static_cast<bGPDstroke *>(gpf_morph->strokes.first);
  int uneq_strokes = 0;

  /* Iterate all strokes in (possibly) morphed frame. */
  for (; gps_morph; gps_morph = gps_morph->next) {
    /* Skip newly created strokes. */
    if (gps_morph->runtime.morph_index == 0) {
      uneq_strokes++;
      continue;
    }
    /* Find matching base stroke. */
    while (gps_base && (gps_base->runtime.morph_index < gps_morph->runtime.morph_index)) {
      gps_base = gps_base->next;
    }
    if (gps_base == nullptr) {
      uneq_strokes++;
      break;
    }
    if (gps_base->runtime.morph_index > gps_morph->runtime.morph_index) {
      uneq_strokes++;
      continue;
    }

    /* Apply existing stroke morph for active morph target. */
    bGPDspoint *pt1;
    float mat[3][3];

    bool morph_found = false;
    smorph.point_deltas = nullptr;

    bGPDsmorph *gpsm = static_cast<bGPDsmorph *>(gps_morph->morphs.first);
    for (; gpsm; gpsm = gpsm->next) {
      if (gpsm->morph_target_nr != active_morph_index) {
        continue;
      }
      morph_found = true;
      smorph.point_deltas = gpsm->point_deltas;

      /* Apply stroke fill color. */
      add_v4_v4(gps_morph->vert_color_fill, gpsm->fill_color_delta);
      clamp_v4(gps_morph->vert_color_fill, 0.0f, 1.0f);

      if (gpsm->point_deltas != nullptr) {
        /* Apply stroke point morphs. */
        for (int i = 0; i < gps_morph->totpoints; i++) {
          bGPDspoint *pt = &gps_morph->points[i];
          bGPDspoint_delta *pd = &gpsm->point_deltas[i];

          /* Convert quaternion rotation to point delta. */
          if (pd->distance > 0.0f) {
            quat_to_mat3(mat, pd->rot_quat);
            if (i < (gps_morph->totpoints - 1)) {
              pt1 = &gps_morph->points[i + 1];
              sub_v3_v3v3(vecb, &pt1->x, &pt->x);
              mul_m3_v3(mat, vecb);
              normalize_v3(vecb);
            }
            else if (gps_morph->totpoints == 1) {
              vecb[0] = 1.0f;
              vecb[1] = 0.0f;
              vecb[2] = 0.0f;
              mul_m3_v3(mat, vecb);
              normalize_v3(vecb);
            }
            mul_v3_v3fl(vecm, vecb, pd->distance);
            add_v3_v3(&pt->x, vecm);
          }

          pt->pressure += pd->pressure;
          clamp_f(pt->pressure, 0.0f, FLT_MAX);
          pt->strength += pd->strength;
          clamp_f(pt->strength, 0.0f, 1.0f);
          add_v4_v4(pt->vert_color, pd->vert_color);
          clamp_v4(pt->vert_color, 0.0f, 1.0f);
        }
      }

      break;
    }

    /* When the number of points in the base stroke and the morph stroke doesn't match,
     * it's difficult to create a morph.
     * For now, we consider the modified stroke a base stroke, without morph.
     * In the future we could implement a smarter algorithm for matching the points.
     */
    if (gps_base->totpoints != gps_morph->totpoints) {
      if (morph_found) {
        if (gpsm->point_deltas != nullptr) {
          MEM_freeN(gpsm->point_deltas);
        }
        BLI_freelinkN(&gps_morph->morphs, gpsm);
      }
      uneq_strokes++;
      break;
    }

    /* Store delta of fill vertex color. */
    sub_v4_v4v4(smorph.fill_color_delta, gps_morph->vert_color_fill, gps_base->vert_color_fill);
    bool stroke_is_morphed = (fabs(smorph.fill_color_delta[0]) > EPSILON) ||
                             (fabs(smorph.fill_color_delta[1]) > EPSILON) ||
                             (fabs(smorph.fill_color_delta[2]) > EPSILON) ||
                             (fabs(smorph.fill_color_delta[3]) > EPSILON);

    /* Restore fill vertex color to base. */
    copy_v4_v4(gps_morph->vert_color_fill, gps_base->vert_color_fill);

    /* Store the delta's between stroke points. */
    for (int i = 0; i < gps_morph->totpoints; i++) {
      bGPDspoint_delta pd;
      bGPDspoint *ptb1;
      bGPDspoint *ptb = &gps_base->points[i];
      bGPDspoint *ptm = &gps_morph->points[i];

      /* Get quaternion rotation and distance between base and morph point. */
      sub_v3_v3v3(vecm, &ptm->x, &ptb->x);
      pd.distance = len_v3(vecm);
      if (pd.distance > 0.0f) {
        if (i < (gps_morph->totpoints - 1)) {
          ptb1 = &gps_base->points[i + 1];
          sub_v3_v3v3(vecb, &ptb1->x, &ptb->x);
          normalize_v3(vecb);
        }
        else if (gps_morph->totpoints == 1) {
          zero_v3(vecb);
          vecb[0] = 1.0f;
        }
        normalize_v3(vecm);
        rotation_between_vecs_to_quat(pd.rot_quat, vecb, vecm);
      }
      else {
        unit_qt(pd.rot_quat);
      }

      /* Get delta's in pressure, strength and vertex color. */
      pd.pressure = ptm->pressure - ptb->pressure;
      pd.strength = ptm->strength - ptb->strength;
      sub_v4_v4v4(pd.vert_color, ptm->vert_color, ptb->vert_color);

      /* Check on difference between morph and base. */
      if ((fabs(pd.distance) > EPSILON) || (fabs(pd.pressure) > EPSILON) ||
          (fabs(pd.strength) > EPSILON) || (fabs(pd.vert_color[0]) > EPSILON) ||
          (fabs(pd.vert_color[1]) > EPSILON) || (fabs(pd.vert_color[2]) > EPSILON) ||
          (fabs(pd.vert_color[3]) > EPSILON))
      {

        if (smorph.point_deltas == nullptr) {
          smorph.point_deltas = (bGPDspoint_delta *)MEM_calloc_arrayN(
              gps_morph->totpoints, sizeof(bGPDspoint_delta), "bGPDsmorph point deltas");
        }
        bGPDspoint_delta *pdm = &smorph.point_deltas[i];
        pdm->distance = pd.distance;
        pdm->pressure = pd.pressure;
        pdm->strength = pd.strength;
        copy_v4_v4(pdm->rot_quat, pd.rot_quat);
        copy_v4_v4(pdm->vert_color, pd.vert_color);

        stroke_is_morphed = true;

        /* Revert to base values, since the delta will be applied by the morph target modifier. */
        ptm->x = ptb->x;
        ptm->y = ptb->y;
        ptm->z = ptb->z;
        ptm->pressure = ptb->pressure;
        ptm->strength = ptb->strength;
        copy_v4_v4(ptm->vert_color, ptb->vert_color);
      }
    }

    /* When there is no difference between morph and base stroke,
     * don't store the morph. */
    if (!stroke_is_morphed) {
      if (morph_found) {
        if (gpsm->point_deltas != nullptr) {
          MEM_freeN(gpsm->point_deltas);
        }
        BLI_freelinkN(&gps_morph->morphs, gpsm);
      }
    }
    else {
      /* Add morph to stroke. */
      if (!morph_found) {
        gpsm = static_cast<bGPDsmorph *>(MEM_mallocN(sizeof(bGPDsmorph), "bGPDsmorph"));
      }
      gpsm->morph_target_nr = active_morph_index;
      gpsm->tot_point_deltas = gps_morph->totpoints;
      gpsm->point_deltas = smorph.point_deltas;
      copy_v4_v4(gpsm->fill_color_delta, smorph.fill_color_delta);

      if (!morph_found) {
        BLI_addtail(&gps_morph->morphs, gpsm);
      }
    }
  }

  return uneq_strokes;
}

void ED_gpencil_morph_target_update_stroke_deltas(MorphTargetsGpencilModifierData *mmd,
                                                  Depsgraph *depsgraph,
                                                  Scene *scene,
                                                  Object *ob)
{
  Object *ob_orig = (Object *)DEG_get_original_id(&ob->id);
  bGPdata *gpd_morph = (bGPdata *)ob_orig->data;

  /* Iterate all layers in morphed GP object. */
  LISTBASE_FOREACH (bGPDlayer *, gpl_morph, &gpd_morph->layers) {
    /* Find matching base layer. */
    bGPDlayer *gpl_base = static_cast<bGPDlayer *>(
        BLI_ghash_lookup(mmd->base_layers, POINTER_FROM_INT(gpl_morph->runtime.morph_index)));
    if (gpl_base == nullptr) {
      continue;
    }

    /* Get active frame. */
    bGPDframe *gpf_morph = BKE_gpencil_frame_retime_get(depsgraph, scene, ob, gpl_morph);
    if (gpf_morph == nullptr) {
      continue;
    }
    bool base_frame_found = false;
    bGPDframe *gpf_base = static_cast<bGPDframe *>(gpl_base->frames.first);
    for (; gpf_base != nullptr; gpf_base = gpf_base->next) {
      if (gpf_base->runtime.morph_index == gpf_morph->runtime.morph_index) {
        base_frame_found = true;
        break;
      }
    }
    if (!base_frame_found) {
      continue;
    }

    /* Create stroke deltas. */
    gpencil_morph_target_create_stroke_deltas(gpf_base, gpf_morph, mmd->index_edited);
  }
}

static void gpencil_morph_target_edit_get_deltas(bContext *C, wmOperator *op)
{
  /* Match the stored base GP object with the morphed one. */
  int uneq_layers = 0, uneq_frames = 0, uneq_strokes = 0;
  bool is_morphed;
  tGPDmorph *tgpm = static_cast<tGPDmorph *>(op->customdata);

  /* Create hash table of morph layers. */
  GHash *morph_layers = BLI_ghash_int_new_ex(__func__, 64);
  LISTBASE_FOREACH (bGPDlayer *, gpl_morph, &tgpm->gpd_morph->layers) {
    BLI_ghash_insert(morph_layers, POINTER_FROM_INT(gpl_morph->runtime.morph_index), gpl_morph);
  }

  /* Iterate all layers in base GP object. */
  LISTBASE_FOREACH (bGPDlayer *, gpl_base, &tgpm->gpd_base->layers) {
    /* Find matching morph layer. */
    bGPDlayer *gpl_morph = static_cast<bGPDlayer *>(
        BLI_ghash_lookup(morph_layers, POINTER_FROM_INT(gpl_base->runtime.morph_index)));
    if (gpl_morph == nullptr) {
      uneq_layers++;
      continue;
    }

    /* Remove existing layer morph for active morph target. */
    bGPDlmorph *gplm = static_cast<bGPDlmorph *>(gpl_morph->morphs.first);
    for (; gplm; gplm = gplm->next) {
      if (gplm->morph_target_nr == tgpm->active_index) {
        BLI_freelinkN(&gpl_morph->morphs, gplm);
        break;
      }
    }

    /* Get delta in layer transformation and opacity. */
    is_morphed = false;
    gplm = static_cast<bGPDlmorph *>(MEM_mallocN(sizeof(bGPDlmorph), "bGPDlmorph"));
    sub_v3_v3v3(gplm->location, gpl_morph->location, gpl_base->location);
    sub_v3_v3v3(gplm->rotation, gpl_morph->rotation, gpl_base->rotation);
    sub_v3_v3v3(gplm->scale, gpl_morph->scale, gpl_base->scale);
    gplm->opacity = gpl_morph->opacity - gpl_base->opacity;

    /* Revert to base values, since the morph was applied during edit. */
    copy_v3_v3(gpl_morph->location, gpl_base->location);
    copy_v3_v3(gpl_morph->rotation, gpl_base->rotation);
    copy_v3_v3(gpl_morph->scale, gpl_base->scale);
    gpl_morph->opacity = gpl_base->opacity;

    /* Get delta in layer order. */
    int gpl_morph_index = BLI_findindex(&tgpm->gpd_morph->layers, gpl_morph) + 1;
    gplm->order = gpl_morph_index - gpl_base->runtime.morph_index;

    /* Revert morph to base order. */
    if (gplm->order != 0) {
      BLI_listbase_move_index(
          &tgpm->gpd_morph->layers, gpl_morph_index - 1, gpl_base->runtime.morph_index - 1);
    }

    /* Check morph on non-zero. */
    if ((gplm->order != 0) || (fabs(gplm->opacity) > EPSILON)) {
      is_morphed = true;
    }
    else {
      for (int i = 0; i < 3; i++) {
        if ((fabs(gplm->location[i]) > EPSILON) || (fabs(gplm->rotation[i]) > EPSILON) ||
            (fabs(gplm->scale[i]) > EPSILON))
        {
          is_morphed = true;
          break;
        }
      }
    }
    /* Don't store a zero morph. */
    if (!is_morphed) {
      MEM_freeN(gplm);
    }
    else {
      /* Add morph to layer. */
      gplm->morph_target_nr = tgpm->active_index;
      BLI_addtail(&gpl_morph->morphs, gplm);
    }

    /* Iterate all frames and strokes. */
    bGPDframe *gpf_base = static_cast<bGPDframe *>(gpl_base->frames.first);
    bGPDframe *gpf_morph = static_cast<bGPDframe *>(gpl_morph->frames.first);
    for (; gpf_morph; gpf_morph = gpf_morph->next) {
      /* Skip newly created frames. */
      if (gpf_morph->runtime.morph_index == 0) {
        uneq_frames++;
        continue;
      }
      /* Find matching base frame. */
      while (gpf_base && (gpf_base->runtime.morph_index < gpf_morph->runtime.morph_index)) {
        gpf_base = gpf_base->next;
      }
      if (gpf_base == nullptr) {
        uneq_frames++;
        break;
      }

      /* Create stroke deltas. */
      uneq_strokes += gpencil_morph_target_create_stroke_deltas(
          gpf_base, gpf_morph, tgpm->active_index);
    }
  }

  /* Free hash memory. */
  BLI_ghash_free(morph_layers, nullptr, nullptr);

  /* Show a warning when there is a mismatch between base and morph. */
  if ((uneq_layers > 0) || (uneq_frames > 0) || (uneq_strokes > 0)) {
    printf("Warning: mismatch between base and morph target after editing '%s' - ",
           tgpm->active_gpmt->name);
    if (uneq_layers > 0) {
      printf("layers: %d ", uneq_layers);
    }
    if (uneq_layers > 0) {
      printf("frames: %d ", uneq_frames);
    }
    if (uneq_layers > 0) {
      printf("strokes: %d ", uneq_strokes);
    }
    printf("\r\n");
  }

  /* Clean up temp data. */
  gpencil_morph_target_edit_exit(C, op);
}

static void gpencil_morph_target_apply_to_layer(bGPDlayer *gpl, bGPDlmorph *gplm, float factor)
{
  for (int i = 0; i < 3; i++) {
    gpl->location[i] += gplm->location[i] * factor;
    gpl->rotation[i] += gplm->rotation[i] * factor;
    gpl->scale[i] += gplm->scale[i] * factor;
  }
  gpl->opacity += gplm->opacity * factor;
  gpl->opacity = clamp_f(gpl->opacity, 0.0f, 1.0f);
  loc_eul_size_to_mat4(gpl->layer_mat, gpl->location, gpl->rotation, gpl->scale);
  invert_m4_m4(gpl->layer_invmat, gpl->layer_mat);
}

static void gpencil_morph_target_edit_init(bContext *C, wmOperator *op)
{
  int layer_index, frame_index, stroke_index;

  tGPDmorph *tgpm = static_cast<tGPDmorph *>(
      MEM_callocN(sizeof(tGPDmorph), "GPencil Morph Target Edit Data"));
  bGPdata *gpd_base = static_cast<bGPdata *>(
      MEM_callocN(sizeof(bGPdata), "Gpencil Morph Target Base"));

  /* Get context attributes. */
  Object *ob = CTX_data_active_object(C);
  bGPdata *gpd = CTX_data_gpencil_data(C);

  /* Get active morph target. */
  bGPDmorph_target *gpmt = BKE_gpencil_morph_target_active_get(gpd);
  tgpm->active_gpmt = gpmt;
  tgpm->active_index = BLI_findindex(&gpd->morph_targets, gpmt);

  /* Get largest 3D viewport in screen. */
  tgpm->area = nullptr;
  tgpm->region = nullptr;
  bScreen *screen = CTX_wm_screen(C);
  int max_w = 0;
  LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
    if (area->spacetype == SPACE_VIEW3D) {
      int w = area->totrct.xmax - area->totrct.xmin;
      if (w > max_w) {
        tgpm->area = area;
        max_w = w;
      }
    }
  }
  if (tgpm->area) {
    LISTBASE_FOREACH (ARegion *, region, &tgpm->area->regionbase) {
      if (region->regiontype == RGN_TYPE_WINDOW) {
        tgpm->region = region;
      }
      if (region->alignment == RGN_ALIGN_TOP && region->regiontype == RGN_TYPE_TOOL_HEADER) {
        tgpm->header_height += (int)(region->sizey * UI_SCALE_FAC + 0.5f);
      }
      if (region->alignment == RGN_ALIGN_RIGHT && region->regiontype == RGN_TYPE_UI) {
        tgpm->npanel_width = region->visible ? 20 * UI_SCALE_FAC : 0;
      }
    }
  }

  /* Set temp operator data. */
  tgpm->gpd_base = gpd_base;
  tgpm->gpd_morph = gpd;
  tgpm->ob = ob;

  /* Store layers, frames, strokes of base GP object. */
  layer_index = 1;
  BLI_listbase_clear(&gpd_base->layers);
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    bGPDlayer *gpl_base = static_cast<bGPDlayer *>(MEM_callocN(sizeof(bGPDlayer), "bGPDlayer"));
    copy_v3_v3(gpl_base->location, gpl->location);
    copy_v3_v3(gpl_base->rotation, gpl->rotation);
    copy_v3_v3(gpl_base->scale, gpl->scale);
    gpl_base->opacity = gpl->opacity;
    gpl->runtime.morph_index = layer_index;
    gpl_base->runtime.morph_index = layer_index++;
    BLI_addtail(&gpd_base->layers, gpl_base);

    /* Apply active morph target to GP object in viewport. */
    LISTBASE_FOREACH (bGPDlmorph *, gplm, &gpl->morphs) {
      if (gplm->morph_target_nr == tgpm->active_index) {
        gpencil_morph_target_apply_to_layer(gpl, gplm, 1.0f);
        gplm->order_applied = 0;
      }
    }

    BLI_listbase_clear(&gpl_base->frames);
    frame_index = 1;
    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      bGPDframe *gpf_base = static_cast<bGPDframe *>(MEM_callocN(sizeof(bGPDframe), "bGPDframe"));
      gpf->runtime.morph_index = frame_index;
      gpf_base->runtime.morph_index = frame_index++;
      BLI_addtail(&gpl_base->frames, gpf_base);

      BLI_listbase_clear(&gpf_base->strokes);
      stroke_index = 1;
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        bGPDstroke *gps_base = static_cast<bGPDstroke *>(
            MEM_callocN(sizeof(bGPDstroke), "bGPDstroke"));
        gps->runtime.morph_index = stroke_index;
        gps_base->runtime.morph_index = stroke_index++;
        BLI_addtail(&gpf_base->strokes, gps_base);
        gps_base->points = static_cast<bGPDspoint *>(MEM_dupallocN(gps->points));
        gps_base->totpoints = gps->totpoints;
        copy_v4_v4(gps_base->vert_color_fill, gps->vert_color_fill);
      }
    }
  }

  /* Apply layer order morph. */
  bGPDlayer *gpl_prev;
  for (bGPDlayer *gpl = static_cast<bGPDlayer *>(gpd->layers.last); gpl; gpl = gpl_prev) {
    gpl_prev = gpl->prev;
    LISTBASE_FOREACH (bGPDlmorph *, gplm, &gpl->morphs) {
      if ((gplm->morph_target_nr == tgpm->active_index) && (gplm->order_applied == 0) &&
          (gplm->order != 0))
      {
        if (!BLI_listbase_link_move(&gpd->layers, gpl, gplm->order)) {
          BLI_remlink(&gpd->layers, gpl);
          if (gplm->order < 0) {
            BLI_addhead(&gpd->layers, gpl);
          }
          else {
            BLI_addtail(&gpd->layers, gpl);
          }
        }
        gplm->order_applied = 1;
      }
    }
  }
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    LISTBASE_FOREACH (bGPDlmorph *, gplm, &gpl->morphs) {
      gplm->order_applied = 0;
    }
  }

  /* Set 'in morph edit mode' flag. */
  in_edit_mode = true;

  /* Mark the edited morph target in the modifiers. */
  bool is_first = true;
  LISTBASE_FOREACH (GpencilModifierData *, md, &tgpm->ob->greasepencil_modifiers) {
    if (md->type == eGpencilModifierType_MorphTargets) {
      MorphTargetsGpencilModifierData *mmd = (MorphTargetsGpencilModifierData *)md;
      mmd->index_edited = tgpm->active_index;
      mmd->gpd_base = (is_first) ? gpd_base : nullptr;
      mmd->base_layers = nullptr;

      /* Create lookup hash table for base layers. */
      if (is_first) {
        mmd->base_layers = BLI_ghash_int_new(__func__);
        LISTBASE_FOREACH (bGPDlayer *, gpl_base, &gpd_base->layers) {
          BLI_ghash_insert(
              mmd->base_layers, POINTER_FROM_INT(gpl_base->runtime.morph_index), gpl_base);
        }
      }

      is_first = false;
    }
  }

  /* Add draw handler to viewport for colored rectangle (marking 'edit mode'). */
  if (tgpm->region != nullptr) {
    tgpm->draw_handle = ED_region_draw_cb_activate(
        tgpm->region->type, gpencil_morph_target_edit_draw, tgpm, REGION_DRAW_POST_PIXEL);
  }

  op->customdata = tgpm;
}

static int gpencil_morph_target_edit_modal(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  /* Operator ends when 'in morph edit mode' flag is disabled (by the Finish Edit operator). */
  if (!in_edit_mode) {
    gpencil_morph_target_edit_get_deltas(C, op);
    return OPERATOR_FINISHED;
  }

  return OPERATOR_PASS_THROUGH;
}

static int gpencil_morph_target_edit_exec(bContext *C, wmOperator *op)
{
  tGPDmorph *tgpm = nullptr;

  /* Initialize temp GP data. */
  gpencil_morph_target_edit_init(C, op);

  /* Push undo for edit morph target. */
  ED_undo_push_op(C, op);

  /* Update GP object with morph target activated. */
  tgpm = static_cast<tGPDmorph *>(op->customdata);
  DEG_id_tag_update(&tgpm->gpd_morph->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, nullptr);

  /* Add a modal handler for this operator. */
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static bool gpencil_morph_target_edit_poll(bContext *C)
{
  if (!gpencil_morph_target_active_poll(C)) {
    return false;
  }

  return !in_edit_mode;
}

void GPENCIL_OT_morph_target_edit(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Edit Morph Target";
  ot->idname = "GPENCIL_OT_morph_target_edit";
  ot->description = "Edit active Grease Pencil morph target";

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* callbacks */
  ot->poll = gpencil_morph_target_edit_poll;
  ot->exec = gpencil_morph_target_edit_exec;
  ot->modal = gpencil_morph_target_edit_modal;
  ot->cancel = gpencil_morph_target_edit_exit;
}

/* ******************* Finish Edit Morph Target ************************ */
static int gpencil_morph_target_edit_finish_exec(bContext * /*C*/, wmOperator * /*op*/)
{
  in_edit_mode = false;
  return OPERATOR_FINISHED;
}

static bool gpencil_morph_target_edit_finish_poll(bContext * /*C*/)
{
  return in_edit_mode;
}

void GPENCIL_OT_morph_target_edit_finish(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Finish Edit Morph Target";
  ot->idname = "GPENCIL_OT_morph_target_edit_finish";
  ot->description = "Finish the editing of the active Grease Pencil morph target";

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Callbacks. */
  ot->poll = gpencil_morph_target_edit_finish_poll;
  ot->exec = gpencil_morph_target_edit_finish_exec;
}

/* ******************* Duplicate Morph Target ************************ */
static int gpencil_morph_target_duplicate_exec(bContext *C, wmOperator *op)
{
  /* Get source. */
  bGPdata *gpd = nullptr;
  Object *ob = CTX_data_active_object(C);
  if ((ob == nullptr) || (ob->type != OB_GPENCIL_LEGACY)) {
    return OPERATOR_CANCELLED;
  }
  gpd = (bGPdata *)ob->data;
  bGPDmorph_target *gpmt = BKE_gpencil_morph_target_active_get(gpd);
  int index_src = BLI_findindex(&gpd->morph_targets, gpmt);
  float value_src = gpmt->value;

  /* Create destination. */
  if (gpencil_morph_target_add_exec(C, op) == OPERATOR_CANCELLED) {
    return OPERATOR_CANCELLED;
  }
  gpmt->value = 0.0f;
  gpmt = BKE_gpencil_morph_target_active_get(gpd);
  int index_dst = BLI_findindex(&gpd->morph_targets, gpmt);
  gpmt->value = value_src;

  /* Copy layer and stroke morph data. */
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    LISTBASE_FOREACH (bGPDlmorph *, gplm, &gpl->morphs) {
      if (gplm->morph_target_nr == index_src) {
        bGPDlmorph *gplm_dst = static_cast<bGPDlmorph *>(MEM_dupallocN(gplm));
        gplm_dst->prev = gplm->next = nullptr;
        gplm_dst->morph_target_nr = index_dst;
        BLI_addtail(&gpl->morphs, gplm_dst);
      }
    }

    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        LISTBASE_FOREACH (bGPDsmorph *, gpsm, &gps->morphs) {
          if (gpsm->morph_target_nr == index_src) {
            bGPDsmorph *gpsm_dst = static_cast<bGPDsmorph *>(MEM_dupallocN(gpsm));
            gpsm_dst->prev = gpsm_dst->next = nullptr;
            gpsm_dst->point_deltas = nullptr;
            if (gpsm->point_deltas != nullptr) {
              gpsm_dst->point_deltas = static_cast<bGPDspoint_delta *>(
                  MEM_dupallocN(gpsm->point_deltas));
            }
            gpsm_dst->morph_target_nr = index_dst;
            BLI_addtail(&gps->morphs, gpsm_dst);
          }
        }
      }
    }
  }

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_morph_target_duplicate(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Duplicate Morph Target";
  ot->idname = "GPENCIL_OT_morph_target_duplicate";
  ot->description = "Duplicate the active Grease Pencil morph target";

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Operator properties. */
  PropertyRNA *prop;
  prop = RNA_def_int(
      ot->srna, "morph_target", 0, -1, INT_MAX, "Grease Pencil Morph Target", "", -1, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_string(
      ot->srna, "name", nullptr, MAX_NAME, "Name", "Name of the newly added morph target");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  ot->prop = prop;

  /* Callbacks. */
  ot->poll = gpencil_morph_target_active_poll;
  ot->exec = gpencil_morph_target_duplicate_exec;
}

/* ******************* Delete All Morph Targets ************************ */
static int gpencil_morph_target_remove_all_exec(bContext *C, wmOperator *op)
{
  bGPdata *gpd = ED_gpencil_data_get_active(C);
  if (gpd == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* Remove all morph data from strokes. */
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    BKE_gpencil_free_layer_morphs(gpl);

    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        BKE_gpencil_free_stroke_morphs(gps);
      }
    }
  }

  /* Update animation data. */
  LISTBASE_FOREACH (bGPDmorph_target *, gpmt, &gpd->morph_targets) {
    char name_esc[sizeof(gpmt->name) * 2];
    char rna_path[sizeof(gpmt->name) * 2 + 32];
    BLI_str_escape(name_esc, gpmt->name, sizeof(name_esc));
    BLI_snprintf(rna_path, sizeof(rna_path), "morph_targets[\"%s\"]", name_esc);
    BKE_animdata_fix_paths_remove(&gpd->id, rna_path);
  }

  /* Remove all morph targets. */
  BLI_freelistN(&gpd->morph_targets);

  /* Remove all morph target modifiers automatically. */
  Object *ob = CTX_data_active_object(C);
  Main *bmain = CTX_data_main(C);
  LISTBASE_FOREACH_MUTABLE (GpencilModifierData *, md, &ob->greasepencil_modifiers) {
    if (md->type != eGpencilModifierType_MorphTargets) {
      continue;
    }
    blender::ed::object::gpencil_modifier_remove(op->reports, bmain, ob, md);
  }

  /* Notifiers. */
  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, nullptr);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, nullptr);

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_morph_target_remove_all(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Remove All Morph Targets";
  ot->idname = "GPENCIL_OT_morph_target_remove_all";
  ot->description = "Remove all morph targets in the Grease Pencil object";

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Callbacks. */
  ot->poll = gpencil_morph_target_active_poll;
  ot->exec = gpencil_morph_target_remove_all_exec;
}

/* ******************* Apply All Morph Targets ************************ */
static int gpencil_morph_target_apply_all_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  Main *bmain = CTX_data_main(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  GpencilModifierData *md_prev = nullptr;

  /* Apply all morph target modifiers in reversed order. */
  for (GpencilModifierData *md =
           static_cast<GpencilModifierData *>(ob->greasepencil_modifiers.last);
       md;
       md = md_prev)
  {
    md_prev = md->prev;
    if (md->type == eGpencilModifierType_MorphTargets) {
      if (!blender::ed::object::gpencil_modifier_apply(bmain, op->reports, depsgraph, ob, md, 0)) {
        return OPERATOR_CANCELLED;
      }
    }
  }

  /* All modifiers applied, now remove all morph targets. */
  return gpencil_morph_target_remove_all_exec(C, op);
}

void GPENCIL_OT_morph_target_apply_all(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Apply All Morph Targets";
  ot->idname = "GPENCIL_OT_morph_target_apply_all";
  ot->description = "Apply all morph targets in the Grease Pencil object";

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Callbacks. */
  ot->poll = gpencil_morph_target_active_poll;
  ot->exec = gpencil_morph_target_apply_all_exec;
}
