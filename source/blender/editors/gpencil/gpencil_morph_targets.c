/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. */

/** \file
 * \ingroup edgpencil
 *
 * Operators for dealing with GP morph targets.
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_string_utils.h"

#include "BLT_translation.h"

#include "DNA_anim_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BKE_anim_data.h"
#include "BKE_animsys.h"
#include "BKE_context.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_report.h"
#include "BKE_scene.h"

#include "WM_api.h"
#include "WM_types.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "ED_gpencil.h"
#include "ED_object.h"
#include "ED_space_api.h"
#include "ED_undo.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "BLF_api.h"
#include "GPU_immediate.h"
#include "GPU_matrix.h"
#include "GPU_state.h"

#include "DEG_depsgraph.h"

#include "gpencil_intern.h"

/* Temporary morph operation data `op->customdata`. */
typedef struct tGPDmorph {
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
} tGPDmorph;

/* State: is a morph target being edited? */
bool in_edit_mode = false;

/* ************************************************ */
/* Morph Target Operators */

/* ******************* Add New Morph Target ************************ */
static int gpencil_morph_target_add_exec(bContext *C, wmOperator *op)
{
  bGPdata *gpd = NULL;

  Object *ob = CTX_data_active_object(C);
  if ((ob != NULL) && (ob->type == OB_GPENCIL)) {
    /* Check maximum number of morph targets. */
    gpd = (bGPdata *)ob->data;
    int count = BLI_listbase_count_at_most(&gpd->morph_targets, GPENCIL_MORPH_TARGETS_MAX);
    if (count >= GPENCIL_MORPH_TARGETS_MAX) {
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
    bGPDmorph_target *gpmt = NULL;
    gpmt = MEM_callocN(sizeof(bGPDmorph_target), "bGPDmorph_target");
    BLI_addtail(&gpd->morph_targets, gpmt);

    gpmt->range_min = 0.0f;
    gpmt->range_max = 1.0f;
    gpmt->value = 0.0f;

    /* Copy values of currently active morph target. */
    bGPDmorph_target *gpmt_act = BKE_gpencil_morph_target_active_get(gpd);
    if (gpmt_act != NULL) {
      if (!name_given) {
        strcpy(name, gpmt_act->name);
      }
      gpmt->range_min = gpmt_act->range_min;
      gpmt->range_max = gpmt_act->range_max;

      /* Increase order index of morph targets after active one. */
      LISTBASE_FOREACH (bGPDmorph_target *, gpmt_sort, &gpd->morph_targets) {
        if (gpmt_sort->order_nr > gpmt_act->order_nr) {
          gpmt_sort->order_nr++;
        }
      }
      gpmt->order_nr = gpmt_act->order_nr + 1;
    }
    else {
      gpmt->order_nr = BLI_listbase_count(&gpd->morph_targets) - 1;
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
    if (md == NULL) {
      Main *bmain = CTX_data_main(C);
      Scene *scene = CTX_data_scene(C);
      md = ED_object_gpencil_modifier_add(
          op->reports, bmain, scene, ob, "Morph Targets", eGpencilModifierType_MorphTargets);
      if (md == NULL) {
        BKE_report(op->reports, RPT_ERROR, "Unable to add a Morph Targets modifier to object");
      }
    }
  }

  /* notifiers */
  if (gpd) {
    DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  }
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, NULL);

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
      ot->srna, "name", NULL, MAX_NAME, "Name", "Name of the newly added morph target");
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
            if (gpsm->point_deltas != NULL) {
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

  /* Lower ui indexes. */
  int order_nr = gpmt->order_nr;
  LISTBASE_FOREACH (bGPDmorph_target *, gpmt_sort, &gpd->morph_targets) {
    if (gpmt_sort->order_nr > order_nr) {
      gpmt_sort->order_nr--;
    }
  }

  /* Update anim data. */
  char name_esc[sizeof(gpmt->name) * 2];
  char rna_path[sizeof(gpmt->name) * 2 + 32];
  BLI_str_escape(name_esc, gpmt->name, sizeof(name_esc));
  BLI_snprintf(rna_path, sizeof(rna_path), "morph_targets[\"%s\"]", name_esc);
  BKE_animdata_fix_paths_remove(&gpd->id, rna_path);

  /* Delete morph target. */
  BLI_freelinkN(&gpd->morph_targets, gpmt);

  /* Set new active morph target. */
  int count = BLI_listbase_count(&gpd->morph_targets);
  if (order_nr == count) {
    order_nr--;
  }
  LISTBASE_FOREACH (bGPDmorph_target *, gpmt_sort, &gpd->morph_targets) {
    if (gpmt_sort->order_nr == order_nr) {
      BKE_gpencil_morph_target_active_set(gpd, gpmt_sort);
      break;
    }
  }

  /* When no morph targets left, remove all morph target modifiers automatically. */
  if (BLI_listbase_count(&gpd->morph_targets) == 0) {
    Object *ob = CTX_data_active_object(C);
    Main *bmain = CTX_data_main(C);

    LISTBASE_FOREACH_MUTABLE (GpencilModifierData *, md, &ob->greasepencil_modifiers) {
      if (md->type != eGpencilModifierType_MorphTargets) {
        continue;
      }
      ED_object_gpencil_modifier_remove(op->reports, bmain, ob, md);
    }
  }

  /* notifiers */
  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, NULL);

  return OPERATOR_FINISHED;
}

bool gpencil_morph_target_active_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if ((ob == NULL) || (ob->type != OB_GPENCIL)) {
    return false;
  }
  bGPdata *gpd = (bGPdata *)ob->data;
  bGPDmorph_target *gpmt = BKE_gpencil_morph_target_active_get(gpd);

  return (gpmt != NULL);
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
  int new_index = gpmt->order_nr + dir;
  if ((new_index < 0) || (new_index >= BLI_listbase_count(&gpd->morph_targets))) {
    return OPERATOR_CANCELLED;
  }

  /* Swap ui order index with neighbour. */
  LISTBASE_FOREACH (bGPDmorph_target *, gpmt_sort, &gpd->morph_targets) {
    if (gpmt_sort->order_nr == new_index) {
      gpmt_sort->order_nr -= dir;
      break;
    }
  }
  gpmt->order_nr = new_index;

  /* Notifiers. */
  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, NULL);

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_morph_target_move(wmOperatorType *ot)
{
  static const EnumPropertyItem morph_target_order_move[] = {
      {-1, "UP", 0, "Up", ""},
      {1, "DOWN", 0, "Down", ""},
      {0, NULL, 0, NULL, NULL},
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
  tGPDmorph *tgpm = op->customdata;

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
                      ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_COPY_ON_WRITE);
    WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, NULL);

    MEM_freeN(tgpm);
  }

  /* Clear 'in morph edit mode' flag. */
  in_edit_mode = false;

  op->customdata = NULL;
}

static void gpencil_morph_target_edit_draw(const bContext *C, ARegion *region, void *arg)
{
  tGPDmorph *tgpm = (tGPDmorph *)arg;
  /* Draw only in the region set by the operator. */
  if (region != tgpm->region) {
    return;
  }

  /* Draw rectangle outline. */
  float half_line_w = 3.0f * UI_DPI_FAC;
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
  BLF_size(font_id, style->widget.points * UI_DPI_FAC);
  BLF_color4fv(font_id, color);
  BLF_enable(font_id, BLF_SHADOW);
  BLF_shadow(font_id, 5, (const float[4]){0.0f, 0.0f, 0.0f, 0.7f});
  BLF_shadow_offset(font_id, 1, -1);

  char text[64] = "Editing Morph Target";
  float x = (rect->xmax - rect->xmin - tgpm->npanel_width) * 0.5f -
            BLF_width(font_id, text, strlen(text)) * 0.5f;
  float y = rect->ymax - rect->ymin - tgpm->header_height - style->widget.points * UI_DPI_FAC -
            half_line_w * 3;
  BLF_position(font_id, x, y, 0);
  BLF_draw(font_id, text, strlen(text));
  BLF_disable(font_id, BLF_SHADOW);
}

static void gpencil_morph_target_edit_get_deltas(bContext *C, wmOperator *op)
{
#define EPSILON 0.00001f

  /* Match the stored base GP object with the morphed one. */
  int uneq_layers = 0, uneq_frames = 0, uneq_strokes = 0;
  bool is_morphed;
  tGPDmorph *tgpm = op->customdata;
  bGPDlayer *gpl_base = tgpm->gpd_base->layers.first;
  bGPDlayer *gpl_morph = tgpm->gpd_morph->layers.first;

  /* Iterate all layers. */
  for (; gpl_morph; gpl_morph = gpl_morph->next) {
    /* Skip newly created layers. */
    if (gpl_morph->runtime.morph_index == 0) {
      uneq_layers++;
      continue;
    }
    /* Find matching base layer. */
    while (gpl_base && (gpl_base->runtime.morph_index < gpl_morph->runtime.morph_index)) {
      gpl_base = gpl_base->next;
    }
    if (gpl_base == NULL) {
      uneq_layers++;
      break;
    }

    /* Remove existing layer morph for active morph target. */
    bGPDlmorph *gplm = gpl_morph->morphs.first;
    for (; gplm; gplm = gplm->next) {
      if (gplm->morph_target_nr == tgpm->active_index) {
        BLI_freelinkN(&gpl_morph->morphs, gplm);
        break;
      }
    }

    /* Get delta in layer transformations. */
    is_morphed = false;
    gplm = MEM_callocN(sizeof(bGPDlmorph), "bGPDlmorph");
    sub_v3_v3v3(gplm->location, gpl_morph->location, gpl_base->location);
    sub_v3_v3v3(gplm->rotation, gpl_morph->rotation, gpl_base->rotation);
    sub_v3_v3v3(gplm->scale, gpl_morph->scale, gpl_base->scale);
    gplm->opacity = gpl_morph->opacity - gpl_base->opacity;

    /* Revert to base values, since the morph was applied during edit. */
    copy_v3_v3(gpl_morph->location, gpl_base->location);
    copy_v3_v3(gpl_morph->rotation, gpl_base->rotation);
    copy_v3_v3(gpl_morph->scale, gpl_base->scale);
    gpl_morph->opacity = gpl_base->opacity;

    /* Check morph on non-zero. */
    if (fabs(gplm->opacity) > EPSILON) {
      is_morphed = true;
    }
    else {
      for (int i = 0; i < 3; i++) {
        if ((fabs(gplm->location[i]) > EPSILON) || (fabs(gplm->rotation[i]) > EPSILON) ||
            (fabs(gplm->scale[i]) > EPSILON)) {
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
    bGPDframe *gpf_base = gpl_base->frames.first;
    bGPDframe *gpf_morph = gpl_morph->frames.first;
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
      if (gpf_base == NULL) {
        uneq_frames++;
        break;
      }

      bGPDstroke *gps_base = gpf_base->strokes.first;
      bGPDstroke *gps_morph = gpf_morph->strokes.first;
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
        if (gps_base == NULL) {
          uneq_strokes++;
          break;
        }

        /* Remove existing morph data for active morph target. */
        bGPDsmorph *gpsm = gps_morph->morphs.first;
        for (; gpsm; gpsm = gpsm->next) {
          if (gpsm->morph_target_nr == tgpm->active_index) {
            if (gpsm->point_deltas != NULL) {
              MEM_freeN(gpsm->point_deltas);
            }
            BLI_freelinkN(&gps_morph->morphs, gpsm);
            break;
          }
        }

        /* When the number of points in the base stroke and the morph stroke doesn't match,
         * it's difficult to create a morph.
         * For now, we consider the modified stroke a base stroke, without morph.
         * In the future we could implement a smarter algorithm for matching the points.
         */
        if (gps_base->totpoints != gps_morph->totpoints) {
          uneq_strokes++;
          break;
        }

        /* Store delta of fill vertex color. */
        gpsm = MEM_callocN(sizeof(bGPDsmorph), "bGPDsmorph");
        sub_v4_v4v4(gpsm->fill_color_delta, gps_morph->vert_color_fill, gps_base->vert_color_fill);
        bool stroke_is_morphed = (fabs(gpsm->fill_color_delta[0]) > EPSILON) ||
                                 (fabs(gpsm->fill_color_delta[1]) > EPSILON) ||
                                 (fabs(gpsm->fill_color_delta[2]) > EPSILON) ||
                                 (fabs(gpsm->fill_color_delta[3]) > EPSILON);
        copy_v4_v4(gps_morph->vert_color_fill, gps_base->vert_color_fill);

        /* Store the delta's between stroke points. */
        is_morphed = false;
        gpsm->point_deltas = MEM_callocN(sizeof(bGPDspoint_delta) * gps_morph->totpoints,
                                         "bGPDsmorph point deltas");
        for (int i = 0; i < gps_morph->totpoints; i++) {
          float vecb[3], vecm[3];
          bGPDspoint *ptb1;
          bGPDspoint *ptb = &gps_base->points[i];
          bGPDspoint *ptm = &gps_morph->points[i];
          bGPDspoint_delta *pd = &gpsm->point_deltas[i];

          /* Get quaternion rotation and distance between base and morph point. */
          sub_v3_v3v3(vecm, &ptm->x, &ptb->x);
          pd->distance = len_v3(vecm);
          if (pd->distance > 0.0f) {
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
            rotation_between_vecs_to_quat(pd->rot_quat, vecb, vecm);
          }
          else {
            unit_qt(pd->rot_quat);
          }

          /* Get delta's in pressure, strength and vertex color. */
          pd->pressure = ptm->pressure - ptb->pressure;
          pd->strength = ptm->strength - ptb->strength;
          sub_v4_v4v4(pd->vert_color, ptm->vert_color, ptb->vert_color);

          /* Revert to base values, since the morph was applied during edit. */
          ptm->x = ptb->x;
          ptm->y = ptb->y;
          ptm->z = ptb->z;
          ptm->pressure = ptb->pressure;
          ptm->strength = ptb->strength;
          copy_v4_v4(ptm->vert_color, ptb->vert_color);

          /* Check on difference between morph and base. */
          if ((fabs(pd->distance) > EPSILON) || (fabs(pd->pressure) > EPSILON) ||
              (fabs(pd->strength) > EPSILON) || (fabs(pd->vert_color[0]) > EPSILON) ||
              (fabs(pd->vert_color[1]) > EPSILON) || (fabs(pd->vert_color[2]) > EPSILON) ||
              (fabs(pd->vert_color[3]) > EPSILON)) {
            is_morphed = true;
            stroke_is_morphed = true;
          }
        }

        /* When there is no difference between morph and base stroke,
         * don't store the morph. */
        if (!is_morphed) {
          MEM_freeN(gpsm->point_deltas);
          gpsm->point_deltas = NULL;
        }
        if (!stroke_is_morphed) {
          MEM_freeN(gpsm);
        }
        else {
          /* Add morph to stroke. */
          gpsm->morph_target_nr = tgpm->active_index;
          gpsm->tot_point_deltas = gps_morph->totpoints;
          BLI_addtail(&gps_morph->morphs, gpsm);
        }
      }
    }
  }

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

#undef EPSILON
}

static void gpencil_morph_target_apply_to_layer(bGPDlayer *gpl, bGPDlmorph *gplm, float factor)
{
  LISTBASE_FOREACH (bGPDlmorph *, gplm, &gpl->morphs) {
    for (int i = 0; i < 3; i++) {
      gpl->location[i] += gplm->location[i] * factor;
      gpl->rotation[i] += gplm->rotation[i] * factor;
      gpl->scale[i] += gplm->scale[i] * factor;
    }
    gpl->opacity += gplm->opacity * factor;
    clamp_f(gpl->opacity, 0.0f, 1.0f);
  }
  loc_eul_size_to_mat4(gpl->layer_mat, gpl->location, gpl->rotation, gpl->scale);
  invert_m4_m4(gpl->layer_invmat, gpl->layer_mat);
}

static void gpencil_morph_target_apply_to_stroke(bGPDstroke *gps, bGPDsmorph *gpsm, float factor)
{
  bGPDspoint *pt1;
  float vecb[3], vecm[3], color_delta[4];
  float mat[3][3];

  copy_v4_v4(color_delta, gpsm->fill_color_delta);
  mul_v4_fl(color_delta, factor);
  add_v4_v4(gps->vert_color_fill, color_delta);
  clamp_v4(gps->vert_color_fill, 0.0f, 1.0f);

  if (gpsm->point_deltas == NULL) {
    return;
  }

  for (int i = 0; i < gps->totpoints; i++) {
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

static void gpencil_morph_target_edit_init(bContext *C, wmOperator *op)
{
  int layer_index, frame_index, stroke_index;

  tGPDmorph *tgpm = MEM_callocN(sizeof(tGPDmorph), "GPencil Morph Target Edit Data");
  bGPdata *gpd_base = MEM_callocN(sizeof(bGPdata), "Gpencil Morph Target Base");

  /* Get context attributes. */
  Object *ob = CTX_data_active_object(C);
  bGPdata *gpd = CTX_data_gpencil_data(C);

  /* Get active morph target. */
  bGPDmorph_target *gpmt = BKE_gpencil_morph_target_active_get(gpd);
  tgpm->active_gpmt = gpmt;
  tgpm->active_index = BLI_findindex(&gpd->morph_targets, gpmt);

  /* Get largest 3D viewport in screen. */
  tgpm->area = NULL;
  tgpm->region = NULL;
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
        tgpm->header_height += (int)(region->sizey * UI_DPI_FAC + 0.5f);
      }
      if (region->alignment == RGN_ALIGN_RIGHT && region->regiontype == RGN_TYPE_UI) {
        tgpm->npanel_width = region->visible ? 20 * UI_DPI_FAC : 0;
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
    bGPDlayer *gpl_base = MEM_callocN(sizeof(bGPDlayer), "bGPDlayer");
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
      }
    }

    BLI_listbase_clear(&gpl_base->frames);
    frame_index = 1;
    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      bGPDframe *gpf_base = MEM_callocN(sizeof(bGPDframe), "bGPDframe");
      gpf->runtime.morph_index = frame_index;
      gpf_base->runtime.morph_index = frame_index++;
      BLI_addtail(&gpl_base->frames, gpf_base);

      BLI_listbase_clear(&gpf_base->strokes);
      stroke_index = 1;
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        bGPDstroke *gps_base = MEM_callocN(sizeof(bGPDstroke), "bGPDstroke");
        gps->runtime.morph_index = stroke_index;
        gps_base->runtime.morph_index = stroke_index++;
        BLI_addtail(&gpf_base->strokes, gps_base);
        gps_base->points = MEM_dupallocN(gps->points);
        gps_base->totpoints = gps->totpoints;
        copy_v4_v4(gps_base->vert_color_fill, gps->vert_color_fill);

        /* Apply active morph target to GP object in viewport. */
        LISTBASE_FOREACH (bGPDsmorph *, gpsm, &gps->morphs) {
          if ((gpsm->morph_target_nr == tgpm->active_index) &&
              (gps->totpoints == gpsm->tot_point_deltas)) {
            gpencil_morph_target_apply_to_stroke(gps, gpsm, 1.0f);
          }
        }
      }
    }
  }

  /* Set 'in morph edit mode' flag. */
  in_edit_mode = true;

  /* Mark the edited morph target in the modifiers. */
  LISTBASE_FOREACH (GpencilModifierData *, md, &tgpm->ob->greasepencil_modifiers) {
    if (md->type == eGpencilModifierType_MorphTargets) {
      MorphTargetsGpencilModifierData *mmd = (MorphTargetsGpencilModifierData *)md;
      mmd->index_edited = tgpm->active_index;
    }
  }

  /* Add draw handler to viewport for colored rectangle (marking 'edit mode'). */
  tgpm->draw_handle = ED_region_draw_cb_activate(
      tgpm->region->type, gpencil_morph_target_edit_draw, tgpm, REGION_DRAW_POST_PIXEL);

  op->customdata = tgpm;
}

static int gpencil_morph_target_edit_modal(bContext *C, wmOperator *op, const wmEvent *event)
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
  tGPDmorph *tgpm = NULL;

  /* Initialize temp GP data. */
  gpencil_morph_target_edit_init(C, op);

  /* Push undo for edit morph target. */
  ED_undo_push_op(C, op);

  /* Update GP object with morph target activated. */
  tgpm = op->customdata;
  DEG_id_tag_update(&tgpm->gpd_morph->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, NULL);

  /* Add a modal handler for this operator. */
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

bool gpencil_morph_target_edit_poll(bContext *C)
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
static int gpencil_morph_target_edit_finish_exec(bContext *C, wmOperator *op)
{
  in_edit_mode = false;
  return OPERATOR_FINISHED;
}

bool gpencil_morph_target_edit_finish_poll(bContext *C)
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
  bGPdata *gpd = NULL;
  Object *ob = CTX_data_active_object(C);
  if ((ob == NULL) || (ob->type != OB_GPENCIL)) {
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
        bGPDlmorph *gplm_dst = MEM_dupallocN(gplm);
        gplm_dst->prev = gplm->next = NULL;
        gplm_dst->morph_target_nr = index_dst;
        BLI_addtail(&gpl->morphs, gplm_dst);
      }
    }

    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        LISTBASE_FOREACH (bGPDsmorph *, gpsm, &gps->morphs) {
          if (gpsm->morph_target_nr == index_src) {
            bGPDsmorph *gpsm_dst = MEM_dupallocN(gpsm);
            gpsm_dst->prev = gpsm_dst->next = NULL;
            gpsm_dst->point_deltas = NULL;
            if (gpsm->point_deltas != NULL) {
              gpsm_dst->point_deltas = MEM_dupallocN(gpsm->point_deltas);
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
      ot->srna, "name", NULL, MAX_NAME, "Name", "Name of the newly added morph target");
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
  if (gpd == NULL) {
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
    ED_object_gpencil_modifier_remove(op->reports, bmain, ob, md);
  }

  /* Notifiers. */
  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, NULL);

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
  GpencilModifierData *md_prev = NULL;

  /* Apply all morph target modifiers in reversed order. */
  for (GpencilModifierData *md = ob->greasepencil_modifiers.last; md; md = md_prev) {
    md_prev = md->prev;
    if (md->type == eGpencilModifierType_MorphTargets) {
      if (!ED_object_gpencil_modifier_apply(bmain, op->reports, depsgraph, ob, md, 0)) {
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
