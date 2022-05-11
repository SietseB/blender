/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. */

/** \file
 * \ingroup edgpencil
 * Operators for Ondine watercolor Grease Pencil.
 */

#include "DNA_gpencil_types.h"
#include "DNA_view3d_types.h"
#include "DNA_layer_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_camera.h"
#include "BKE_context.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_geom.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_scene.h"

#include "BLI_listbase.h"
#include "BLI_math.h"

#include "ED_view3d.h"

#include "WM_api.h"
#include "WM_types.h"

#include "io_gpencil.h"

#include "ED_gpencil.h"
#include "gpencil_intern.h"

#include "gpencil_ondine.hh"

// using blender::editor::gpencil::GpencilOndine;

namespace blender::editor::gpencil {

GpencilOndine *ondine_render = new GpencilOndine();

/* Runtime render properties */
GpencilOndine::GpencilOndine()
{
}

void GpencilOndine::init(bContext *C)
{
  /* Easy access data. */
  Main *bmain_ = CTX_data_main(C);
  Depsgraph *depsgraph_ = CTX_data_depsgraph_pointer(C);
  Scene *scene_ = CTX_data_scene(C);
  ARegion *region_ = get_invoke_region(C);
  View3D *v3d_ = get_invoke_view3d(C);
  RegionView3D *rv3d_ = (RegionView3D *)region_->regiondata;

  UNUSED_VARS(bmain_, depsgraph_, scene_, region_, v3d_, rv3d_);
}

bool GpencilOndine::prepare_camera_params(bContext *C)
{
  Object *cam_ob = scene_->camera;

  /* Calculate camera matrix. */
  if (cam_ob != nullptr) {
    /* Set up parameters. */
    CameraParams params;
    BKE_camera_params_init(&params);
    BKE_camera_params_from_object(&params, cam_ob);

    /* Compute matrix, view-plane, etc. */
    RenderData *rd = &scene_->r;
    BKE_camera_params_compute_viewplane(&params, rd->xsch, rd->ysch, rd->xasp, rd->yasp);
    BKE_camera_params_compute_matrix(&params);

    float viewmat[4][4];
    invert_m4_m4(viewmat, cam_ob->obmat);

    mul_m4_m4m4(persmat_, params.winmat, viewmat);
  }
  else {
    unit_m4(persmat_);
  }

  winx_ = region_->winx;
  winy_ = region_->winy;

  /* Camera rectangle. */
  if (rv3d_->persp == RV3D_CAMOB) {
    render_x_ = (scene_->r.xsch * scene_->r.size) / 100;
    render_y_ = (scene_->r.ysch * scene_->r.size) / 100;

    ED_view3d_calc_camera_border(CTX_data_scene(C),
                                 depsgraph_,
                                 region_,
                                 v3d_,
                                 rv3d_,
                                 &camera_rect_,
                                 true);
    is_camera_ = true;
    camera_ratio_ = render_x_ / (camera_rect_.xmax - camera_rect_.xmin);
    offset_.x = camera_rect_.xmin;
    offset_.y = camera_rect_.ymin;
    return true;
  }
  else {
    return false;
  }
}

void GpencilOndine::set_zdepths(bContext *C)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);

  float3 camera_z_axis;
  copy_v3_v3(camera_z_axis, rv3d_->viewinv[2]);

  /* Iterate all objects in active view layer */
  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    Object *object = base->object;

    /* Grease pencil object? */
    if (object->type != OB_GPENCIL) {
      continue;
    }

    /* Ondine watercolor object? */
    bGPdata *gpd = (bGPdata *)object->data;
    if ((gpd->ondine_flag & GP_ONDINE_WATERCOLOR) == 0) {
      continue;
    }

    /* Save z-depth from view to sort from back to front. */
    gpd->render_zdepth = dot_v3v3(camera_z_axis, object->obmat[3]);
  }
}

}  // namespace blender::editor::gpencil

/* -------------------------------------------------------------------- */
/** \name Set z-depths
 * \{ */

/* Poll for GP watercolor objects */
static bool gpencil_ondine_set_zdepths_poll(bContext *C)
{
  return true;
}

static int gpencil_ondine_set_zdepths_exec(bContext *C, wmOperator *op)
{
  /* Get camera settings */
  /*
  blender::editor::gpencil::ondine_render->init(C);
  bool succes = blender::editor::gpencil::ondine_render->prepare_camera_params(C);
  if (!succes) {
    return OPERATOR_CANCELLED;
  }
  */

  /* Get list of GP watercolor objects */
  // blender::editor::gpencil::ondine_render->set_zdepths(C);

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_ondine_set_zdepths(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Set GP object z-depths";
  ot->idname = "GPENCIL_OT_ondine_set_zdepths";
  ot->description = "Set z-depths of grease pencil watercolor objects";

  /* api callbacks */
  ot->exec = gpencil_ondine_set_zdepths_exec;
  ot->poll = gpencil_ondine_set_zdepths_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER;
}

/** \} */
