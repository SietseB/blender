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

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_ghash.h"
#include "BLI_math.h"
#include "BLI_string_utils.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "DNA_anim_types.h"
#include "DNA_brush_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_material_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BKE_anim_data.h"
#include "BKE_animsys.h"
#include "BKE_brush.h"
#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_fcurve_driver.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_report.h"
#include "BKE_scene.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "WM_api.h"
#include "WM_types.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "ED_gpencil.h"
#include "ED_object.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_query.h"

#include "gpencil_intern.h"

/* Temporary morph operation data `op->customdata`. */
typedef struct tGPDmorph {
  bContext *C;
  struct Main *bmain;
  struct Depsgraph *depsgraph;
  /** window where painting originated */
  struct wmWindow *win;
  /** current scene from context */
  struct Scene *scene;
  /** current active gp object */
  struct Object *ob;
  /** area where painting originated */
  struct ScrArea *area;
  /** region where painting originated */
  struct RegionView3D *rv3d;
  /** view3 where painting originated */
  struct View3D *v3d;
  /** region where painting originated */
  struct ARegion *region;
  /** Base GP data-block. */
  struct bGPdata *gpd_base;
  /** Morph target GP data-block. */
  struct bGPdata *gpd_morph;
} tGPDmorph;

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
      BKE_report(op->reports, RPT_ERROR, "Maximum number of morph targets reached");
      return OPERATOR_CANCELLED;
    }

    /* Get name. */
    PropertyRNA *prop;
    char name[128];
    prop = RNA_struct_find_property(op->ptr, "new_morph_target_name");
    if (RNA_property_is_set(op->ptr, prop)) {
      RNA_property_string_get(op->ptr, prop, name);
    }
    else {
      strcpy(name, "Morph");
    }

    bGPDmorph_target *gpmt = NULL;
    gpmt = MEM_callocN(sizeof(bGPDmorph_target), "bGPDmorph_target");
    BLI_addtail(&gpd->morph_targets, gpmt);

    gpmt->group_nr = 0;
    gpmt->range_min = 0.0f;
    gpmt->range_max = 1.0f;
    gpmt->value = 0.0f;

    /* auto-name */
    BLI_strncpy(gpmt->name, DATA_(name), sizeof(gpmt->name));
    BLI_uniquename(&gpd->morph_targets,
                   gpmt,
                   DATA_("Morph"),
                   '.',
                   offsetof(bGPDmorph_target, name),
                   sizeof(gpmt->name));
    /* set active */
    BKE_gpencil_morph_target_active_set(gpd, gpmt);
  }

  /* notifiers */
  if (gpd) {
    DEG_id_tag_update(&gpd->id,
                      ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_COPY_ON_WRITE);
  }
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, NULL);

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_morph_target_add(wmOperatorType *ot)
{
  PropertyRNA *prop;
  /* identifiers */
  ot->name = "Add New Morph Target";
  ot->idname = "GPENCIL_OT_morph_target_add";
  ot->description = "Add new morph target for the active data-block";

  /* callbacks */
  ot->exec = gpencil_morph_target_add_exec;
  ot->poll = gpencil_add_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop = RNA_def_int(
      ot->srna, "morph_target", 0, -1, INT_MAX, "Grease Pencil Morph Target", "", -1, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_string(ot->srna,
                        "new_morph_target_name",
                        NULL,
                        MAX_NAME,
                        "Name",
                        "Name of the newly added morph target");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  ot->prop = prop;
}

/* ******************* Remove Morph Target ************************ */
static int gpencil_morph_target_remove_exec(bContext *C, wmOperator *op)
{
  bGPdata *gpd = ED_gpencil_data_get_active(C);
  bGPDmorph_target *gpmt = BKE_gpencil_morph_target_active_get(gpd);

  /* Remember index of removed morph target. */
  int index = BLI_findindex(&gpd->morph_targets, gpmt);

  /* Set new active morph target. */
  if (gpmt->prev) {
    BKE_gpencil_morph_target_active_set(gpd, gpmt->prev);
  }
  else {
    BKE_gpencil_morph_target_active_set(gpd, gpmt->next);
  }

  /* Delete morph target data from all strokes and
   * lower the indexes higher than the morph target index
   * to be removed. */
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        LISTBASE_FOREACH_MUTABLE (bGPDsmorph *, gpsm, &gps->morphs) {
          if (gpsm->morph_target_nr == index) {
            MEM_freeN(gpsm->point_deltas);
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

  /* Delete morph target. */
  BLI_freelinkN(&gpd->morph_targets, gpmt);

  /* notifiers */
  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, NULL);

  return OPERATOR_FINISHED;
}

bool gpencil_morph_target_remove_poll(bContext *C)
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
  ot->poll = gpencil_morph_target_remove_poll;
}
